/*-------------------------------------------------------------------------
 *
 * ts_shared.c
 *	  tsearch shared memory management
 *
 * Portions Copyright (c) 1996-2018, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/tsearch/ts_shared.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "lib/dshash.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "tsearch/ts_shared.h"
#include "utils/hashutils.h"
#include "utils/memutils.h"


/*
 * Hash table entries representing shared dictionaries.
 */
typedef struct
{
	Oid			dict_id;
	dsm_handle	dict_dsm;
	Size		dict_size;

	/* How many backends have DSM mapping */
	uint32		refcnt;
} TsearchDictEntry;

static dshash_table *dict_table = NULL;

/*
 * Information about the main shmem segment, used to coordinate
 * access to the hash table and dictionaries.
 */
typedef struct
{
	dsa_handle	area;
	dshash_table_handle dict_table_handle;

	/* Total size of loaded dictionaries into shared memory in bytes */
	Size		loaded_size;

	LWLock		lock;
} TsearchCtlData;

static TsearchCtlData *tsearch_ctl;

/*
 * Maximum allowed amount of shared memory for shared dictionaries,
 * in kilobytes. Default value is 100MB.
 */
int			max_shared_dictionaries_size = 100 * 1024;

static void init_dict_table(void);

/* Parameters for dict_table */
static const dshash_parameters dict_table_params ={
	sizeof(Oid),
	sizeof(TsearchDictEntry),
	dshash_memcmp,
	dshash_memhash,
	LWTRANCHE_TSEARCH_TABLE
};

/*
 * Build the dictionary using allocate_cb callback. If
 * max_shared_dictionaries_size is TS_DICT_SHMEM_UNLIMITED or if there is a
 * space in shared memory and max_shared_dictionaries_size is greater than 0
 * copy the dictionary into DSM.
 *
 * If max_shared_dictionaries_size isn't 0 then try to find the dictionary in
 * shared hash table first. If it was built by someone earlier just return its
 * location in DSM.
 *
 * initoptions: an argument used within a template's init method.
 * allocate_cb: function to build the dictionary, if it wasn't found in DSM.
 *
 * Returns address in the dynamic shared memory segment or in backend memory.
 */
void *
ts_dict_shmem_location(DictInitData *initoptions,
					   ts_dict_build_callback allocate_cb)
{
	TsearchDictEntry *entry;
	bool		found;
	dsm_segment *seg;
	void	   *dict,
			   *dict_location;

#define CHECK_SHARED_SPACE() \
	if (max_shared_dictionaries_size != TS_DICT_SHMEM_UNLIMITED	&& \
		entry->dict_size + tsearch_ctl->loaded_size >	\
		max_shared_dictionaries_size * 1024L)			\
	{													\
		LWLockRelease(&tsearch_ctl->lock);				\
		ereport(LOG, \
				(errmsg("there is no space in shared memory for text search " \
						"dictionary %u, it will be loaded into backend's memory", \
						initoptions->dictid))); \
		dshash_delete_entry(dict_table, entry);		\
		return dict; \
	} \

	init_dict_table();

	/*
	 * Build the dictionary in backend's memory if a hash table wasn't created
	 * or dictid is invalid (it may happen if the dicionary's init method was
	 * called within verify_dictoptions()).
	 */
	if (!DsaPointerIsValid(tsearch_ctl->dict_table_handle) ||
		!OidIsValid(initoptions->dictid))
	{
		Size		dict_size;

		dict = allocate_cb(initoptions->dictoptions, &dict_size);

		return dict;
	}

	/* Try to find an entry in the hash table */
	entry = (TsearchDictEntry *) dshash_find(dict_table, &initoptions->dictid,
											 false);

	if (entry)
	{
		seg = dsm_find_mapping(entry->dict_dsm);
		if (!seg)
		{
			seg = dsm_attach(entry->dict_dsm);
			/* Remain attached until end of session */
			dsm_pin_mapping(seg);
		}

		entry->refcnt++;
		dshash_release_lock(dict_table, entry);

		return dsm_segment_address(seg);
	}

	/* Dictionary haven't been loaded into memory yet */
	entry = (TsearchDictEntry *) dshash_find_or_insert(dict_table,
													   &initoptions->dictid,
													   &found);

	if (found)
	{
		/*
		 * Someone concurrently inserted a dictionary entry since the first time
		 * we checked.
		 */
		seg = dsm_attach(entry->dict_dsm);

		/* Remain attached until end of session */
		dsm_pin_mapping(seg);

		entry->refcnt++;
		dshash_release_lock(dict_table, entry);

		return dsm_segment_address(seg);
	}

	/* Build the dictionary */
	dict = allocate_cb(initoptions->dictoptions, &entry->dict_size);

	LWLockAcquire(&tsearch_ctl->lock, LW_SHARED);

	/* We can only get here when shared dictionaries are enabled */
	Assert(max_shared_dictionaries_size > 0 ||
		   max_shared_dictionaries_size == TS_DICT_SHMEM_UNLIMITED);

	/* Before allocating a DSM segment check remaining shared space */
	CHECK_SHARED_SPACE();

	LWLockRelease(&tsearch_ctl->lock);
	/* If we come here, we need an exclusive lock */
	while (!LWLockAcquireOrWait(&tsearch_ctl->lock, LW_EXCLUSIVE))
	{
		/*
		 * Check again in case if there are no space anymore while we were
		 * waiting for exclusive lock.
		 */
		CHECK_SHARED_SPACE();
	}

	tsearch_ctl->loaded_size += entry->dict_size;

	LWLockRelease(&tsearch_ctl->lock);

	/* At least, allocate a DSM segment for the compiled dictionary */
	seg = dsm_create(entry->dict_size, 0);
	dict_location = dsm_segment_address(seg);
	memcpy(dict_location, dict, entry->dict_size);

	pfree(dict);

	entry->dict_id = initoptions->dictid;
	entry->dict_dsm = dsm_segment_handle(seg);
	entry->refcnt++;

	/* Remain attached until end of postmaster */
	dsm_pin_segment(seg);
	/* Remain attached until end of session */
	dsm_pin_mapping(seg);

	dshash_release_lock(dict_table, entry);

	return dsm_segment_address(seg);
}

/*
 * Release memory occupied by the dictionary. Function just unpins DSM mapping.
 * If nobody else hasn't mapping to this DSM then unpin DSM segment.
 *
 * dictid: Oid of the dictionary.
 */
void
ts_dict_shmem_release(Oid dictid)
{
	TsearchDictEntry *entry;

	/*
	 * If we didn't attach to a hash table then do nothing.
	 */
	if (!dict_table)
		return;

	/* Try to find an entry in the hash table */
	entry = (TsearchDictEntry *) dshash_find(dict_table, &dictid, true);

	if (entry)
	{
		dsm_segment *seg;

		seg = dsm_find_mapping(entry->dict_dsm);
		/*
		 * If current backend didn't pin a mapping then we don't need to do
		 * unpinning.
		 */
		if (!seg)
		{
			dshash_release_lock(dict_table, entry);
			return;
		}

		dsm_unpin_mapping(seg);
		dsm_detach(seg);

		entry->refcnt--;

		if (entry->refcnt == 0)
		{
			dsm_unpin_segment(entry->dict_dsm);

			/* Decrease total loaded size */
			LWLockAcquire(&tsearch_ctl->lock, LW_EXCLUSIVE);
			tsearch_ctl->loaded_size -= entry->dict_size;
			LWLockRelease(&tsearch_ctl->lock);

			dshash_delete_entry(dict_table, entry);
		}
		else
			dshash_release_lock(dict_table, entry);
	}
}

/*
 * Allocate and initialize tsearch-related shared memory.
 */
void
TsearchShmemInit(void)
{
	bool		found;

	tsearch_ctl = (TsearchCtlData *)
		ShmemInitStruct("Full Text Search Ctl", sizeof(TsearchCtlData), &found);

	if (!found)
	{
		LWLockRegisterTranche(LWTRANCHE_TSEARCH_DSA, "tsearch_dsa");
		LWLockRegisterTranche(LWTRANCHE_TSEARCH_TABLE, "tsearch_table");

		LWLockInitialize(&tsearch_ctl->lock, LWTRANCHE_TSEARCH_DSA);

		tsearch_ctl->area = DSM_HANDLE_INVALID;
		tsearch_ctl->dict_table_handle = InvalidDsaPointer;
		tsearch_ctl->loaded_size = 0;
	}
}

/*
 * Report shared memory space needed by TsearchShmemInit.
 */
Size
TsearchShmemSize(void)
{
	Size		size = 0;

	/* size of service structure */
	size = add_size(size, MAXALIGN(sizeof(TsearchCtlData)));

	return size;
}

/*
 * Initialize hash table located in DSM.
 *
 * The hash table should be created and initialized iff
 * max_shared_dictionaries_size GUC isn't equal to zero and it doesn't exist
 * yet.
 */
static void
init_dict_table(void)
{
	MemoryContext old_context;
	dsa_area   *dsa;

	/* Bail out if shared dictionaries not allowed */
	if (max_shared_dictionaries_size == 0)
		return;

	if (dict_table)
		return;

	old_context = MemoryContextSwitchTo(TopMemoryContext);

recheck_table:
	LWLockAcquire(&tsearch_ctl->lock, LW_SHARED);

	/* Hash table have been created already by someone */
	if (DsaPointerIsValid(tsearch_ctl->dict_table_handle))
	{
		Assert(tsearch_ctl->area != DSM_HANDLE_INVALID);

		dsa = dsa_attach(tsearch_ctl->area);

		dict_table = dshash_attach(dsa,
								   &dict_table_params,
								   tsearch_ctl->dict_table_handle,
								   NULL);
	}
	else
	{
		/* Try to get exclusive lock */
		LWLockRelease(&tsearch_ctl->lock);
		if (!LWLockAcquireOrWait(&tsearch_ctl->lock, LW_EXCLUSIVE))
		{
			/*
			 * The lock was released by another backend and other backend
			 * has concurrently created the hash table already.
			 */
			goto recheck_table;
		}

		dsa = dsa_create(LWTRANCHE_TSEARCH_DSA);
		tsearch_ctl->area = dsa_get_handle(dsa);

		dict_table = dshash_create(dsa, &dict_table_params, NULL);
		tsearch_ctl->dict_table_handle = dshash_get_hash_table_handle(dict_table);

		/* Remain attached until end of postmaster */
		dsa_pin(dsa);
	}

	LWLockRelease(&tsearch_ctl->lock);

	/* Remain attached until end of session */
	dsa_pin_mapping(dsa);

	MemoryContextSwitchTo(old_context);
}
