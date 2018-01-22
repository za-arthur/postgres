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

#include "fmgr.h"
#include "lib/dshash.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "tsearch/ts_shared.h"
#include "utils/fmgrprotos.h"
#include "utils/hashutils.h"
#include "utils/memutils.h"


/*
 * Hash table structures
 */
typedef struct
{
	Oid			dict_id;
	dsa_pointer	dict_pointer;
} TsearchDictEntry;

static dshash_table *dict_table = NULL;

/* Area in shared memory for allocating chunks for dictionaries */
static dsa_area *dict_dsa = NULL;

/*
 * Shared struct for locking
 */
typedef struct
{
	dsa_handle	table_area;
	dshash_table_handle dict_table_handle;

	dsa_handle	dict_area;

	/* Total size of loaded dictionaries into shared memory in bytes */
	Size		loaded_size;

	LWLock		lock;
} TsearchCtlData;

static TsearchCtlData *tsearch_ctl;

/*
 * GUC variable for maximum number of shared dictionaries. Default value is
 * 100MB.
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
 * Build the dictionary using allocate_cb callback. If there is a space in
 * shared memory and max_shared_dictionaries_size is greater than 0 copy the
 * dictionary into DSM.
 *
 * If max_shared_dictionaries_size is greater than 0 then try to find the
 * dictionary in shared hash table first. If it was built by someone earlier
 * just return its location in DSM.
 *
 * dictid: Oid of the dictionary.
 * arg: an argument to the callback function.
 * allocate_cb: function to build the dictionary, if it wasn't found in DSM.
 *
 * Returns address in the dynamic shared memory segment or in backend memory.
 */
void *
ts_dict_shmem_location(Oid dictid, List *dictoptions,
					   ispell_build_callback allocate_cb)
{
	TsearchDictEntry *entry;
	bool		found;
	void	   *dict,
			   *dict_location;
	Size		dict_size;

#define CHECK_SHARED_SPACE() \
	if (dict_size + tsearch_ctl->loaded_size >		\
		max_shared_dictionaries_size * 1024L)		\
	{												\
		LWLockRelease(&tsearch_ctl->lock);			\
		ereport(LOG, \
				(errmsg("there is no space in shared memory for text search " \
						"dictionary %u, it will be loaded into backend's memory", \
						dictid))); \
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
		!OidIsValid(dictid))
	{
		dict = allocate_cb(dictoptions, &dict_size);

		return dict;
	}

	/* Try to find an entry in the hash table */
	entry = (TsearchDictEntry *) dshash_find(dict_table, &dictid, false);

	if (entry)
	{
		dict_location = dsa_get_address(dict_dsa, entry->dict_pointer);

		dshash_release_lock(dict_table, entry);

		return dict_location;
	}

	/* Dictionary haven't been loaded into memory yet */
	entry = (TsearchDictEntry *) dshash_find_or_insert(dict_table, &dictid,
													   &found);

	if (found)
	{
		/*
		 * Someone concurrently inserted a dictionary entry since the first time
		 * we checked.
		 */
		dict_location = dsa_get_address(dict_dsa, entry->dict_pointer);

		dshash_release_lock(dict_table, entry);

		return dict_location;
	}

	/* Build the dictionary */
	dict = allocate_cb(dictoptions, &dict_size);

	LWLockAcquire(&tsearch_ctl->lock, LW_SHARED);

	/* Before allocating a DSM segment check remaining shared space */
	Assert(max_shared_dictionaries_size);

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

	tsearch_ctl->loaded_size += dict_size;

	LWLockRelease(&tsearch_ctl->lock);

	/* At least, allocate shared memory for the compiled dictionary */
	entry->dict_id = dictid;
	entry->dict_pointer = dsa_allocate(dict_dsa, dict_size);

	dict_location = dsa_get_address(dict_dsa, entry->dict_pointer);
	memcpy(dict_location, dict, dict_size);

	pfree(dict);

	dshash_release_lock(dict_table, entry);

	return dict_location;
}

/*
 * Release space in shared memory occupied by the dictionary.
 *
 * dictid: Oid of the dictionary.
 *
 * Returns true if the dictionary was loaded into shared memory before and
 * successfully released. Returns false if the dictionary wasn't loaded into
 * shared memory before.
 */
bool
ts_dict_unload(Oid dictid)
{
	TsearchDictEntry *entry;

	init_dict_table();

	/*
	 * Return false if a hash table wasn't created.
	 */
	if (!DsaPointerIsValid(tsearch_ctl->dict_table_handle))
		return false;

	/* Try to find an entry in the hash table */
	entry = (TsearchDictEntry *) dshash_find(dict_table, &dictid, true);

	if (entry)
	{
		dsa_free(dict_dsa, entry->dict_pointer);

		dshash_delete_entry(dict_table, entry);

		return true;
	}

	return false;
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

		tsearch_ctl->table_area = DSM_HANDLE_INVALID;
		tsearch_ctl->dict_table_handle = InvalidDsaPointer;
		tsearch_ctl->dict_area = DSM_HANDLE_INVALID;
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

	/* size of lookup hash table */
	size = add_size(size, hash_estimate_size(max_shared_dictionaries_size,
											 sizeof(TsearchDictEntry)));

	return size;
}

/*
 * Initialize hash table located in DSM.
 *
 * The hash table should be created and initialized iff
 * max_shared_dictionaries_size GUC is greater than zero and it doesn't exist
 * yet.
 */
static void
init_dict_table(void)
{
	MemoryContext old_context;
	dsa_area   *dsa;

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
		Assert(tsearch_ctl->table_area != DSM_HANDLE_INVALID);
		Assert(tsearch_ctl->dict_area != DSM_HANDLE_INVALID);

		dsa = dsa_attach(tsearch_ctl->table_area);

		dict_table = dshash_attach(dsa,
								   &dict_table_params,
								   tsearch_ctl->dict_table_handle,
								   NULL);

		dict_dsa = dsa_attach(tsearch_ctl->dict_area);
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
		tsearch_ctl->table_area = dsa_get_handle(dsa);

		dict_table = dshash_create(dsa, &dict_table_params, NULL);
		tsearch_ctl->dict_table_handle = dshash_get_hash_table_handle(dict_table);

		/* Remain attached until end of postmaster */
		dsa_pin(dsa);

		dict_dsa = dsa_create(LWTRANCHE_TSEARCH_DSA);
		tsearch_ctl->dict_area = dsa_get_handle(dict_dsa);

		/* Remain attached until end of postmaster */
		dsa_pin(dict_dsa);
	}

	LWLockRelease(&tsearch_ctl->lock);

	/* Remain attached until end of session */
	dsa_pin_mapping(dsa);

	/* Remain attached until end of session */
	dsa_pin_mapping(dict_dsa);

	MemoryContextSwitchTo(old_context);
}

/*
 * Release memory occupied by the dictionary. Uses TS template's unload method.
 */
Datum
ts_unload(PG_FUNCTION_ARGS)
{
	Oid			dictid = PG_GETARG_OID(0);

	PG_RETURN_BOOL(ts_dict_unload(dictid));
}

/*
 * Reload the dictionary using TS template's reload method.
 */
//Datum
//ts_reload(PG_FUNCTION_ARGS)
//{
//	PG_RETURN_POINTER(NULL);
//}
