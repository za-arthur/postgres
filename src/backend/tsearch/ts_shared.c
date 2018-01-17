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


/*
 * Hash table structures
 */

typedef struct
{
	char		dictfile[MAXPGPATH];
	char		afffile[MAXPGPATH];
} TsearchDictKey;

typedef struct
{
	TsearchDictKey key;
	dsm_handle	dict_dsm;
} TsearchDictEntry;

static dshash_table *dict_table = NULL;

/*
 * Shared struct for locking
 */
typedef struct
{
	dsa_handle	area;
	dshash_table_handle dict_table_handle;

	LWLock		lock;
} TsearchCtlData;

static TsearchCtlData *tsearch_ctl;

/*
 * GUC variable for maximum number of shared dictionaries
 */
int			max_shared_dictionaries_size = 10;

static void init_dict_table(void);

static int dict_table_compare(const void *a, const void *b, size_t size,
							  void *arg);
static uint32 dict_table_hash(const void *a, size_t size, void *arg);

/* Parameters for dict_table */
static const dshash_parameters dict_table_params ={
	sizeof(TsearchDictKey),
	sizeof(TsearchDictEntry),
	dict_table_compare,
	dict_table_hash,
	LWTRANCHE_TSEARCH_TABLE
};

/*
 * Get location in shared memory using hash table. If shared memory for
 * dictfile and afffile doesn't allocated yet, do it.
 *
 * dictbuild: building structure for the dictionary.
 * dictfile: .dict file of the dictionary.
 * afffile: .aff file of the dictionary.
 * allocate_cb: function to build the dictionary, if it wasn't found in DSM.
 *
 * Returns address in the dynamic shared memory segment or NULL if there is no
 * space in shared hash table.
 */
void *
ispell_shmem_location(void *dictbuild,
					  const char *dictfile, const char *afffile,
					  ispell_build_callback allocate_cb)
{
	TsearchDictKey key;
	TsearchDictEntry *entry;
	bool		found;
	dsm_segment *seg;
	void	   *ispell_dict,
			   *dict_location;
	Size		ispell_size;

	init_dict_table();

	/* If hash table wasn't created then do nothing */
	if (!DsaPointerIsValid(tsearch_ctl->dict_table_handle))
		return NULL;

	StrNCpy(key.dictfile, dictfile, MAXPGPATH);
	StrNCpy(key.afffile, afffile, MAXPGPATH);

	/* Try to find an entry in the hash table */
	entry = (TsearchDictEntry *) dshash_find(dict_table, &key, false);

	if (entry)
	{
		seg = dsm_attach(entry->dict_dsm);

		/* Remain attached until end of session */
		dsm_pin_mapping(seg);

		dshash_release_lock(dict_table, entry);

		return dsm_segment_address(seg);
	}

	/* Dictionary haven't been loaded into memory yet */
	entry = (TsearchDictEntry *) dshash_find_or_insert(dict_table, &key,
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

		dshash_release_lock(dict_table, entry);

		return dsm_segment_address(seg);
	}

	/* Build the dictionary */
	ispell_dict = allocate_cb(dictbuild, dictfile, afffile, &ispell_size);

	seg = dsm_create(ispell_size, 0);
	dict_location = dsm_segment_address(seg);
	memcpy(dict_location, ispell_dict, ispell_size);

	pfree(ispell_dict);

	entry->dict_dsm = dsm_segment_handle(seg);

	/* Remain attached until end of postmaster */
	dsm_pin_segment(seg);
	/* Remain attached until end of session */
	dsm_pin_mapping(seg);

	dshash_release_lock(dict_table, entry);

	return dsm_segment_address(seg);
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

		tsearch_ctl->dict_table_handle = InvalidDsaPointer;
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
	dsa_area   *dsa;

	if (max_shared_dictionaries_size == 0)
		return;

recheck_table:
	LWLockAcquire(&tsearch_ctl->lock, LW_SHARED);

	/* Hash table have been created already by someone */
	if (DsaPointerIsValid(tsearch_ctl->dict_table_handle))
	{
		Assert(DsaPointerIsValid(tsearch_ctl->area));

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
	}

	LWLockRelease(&tsearch_ctl->lock);

	/* Remain attached until end of postmaster */
	dsa_pin(dsa);
}

/*
 * A comparator function for TsearchDictKey.
 */
static int
dict_table_compare(const void *a, const void *b, size_t size, void *arg)
{
	TsearchDictKey *k1 = (TsearchDictKey *) a;
	TsearchDictKey *k2 = (TsearchDictKey *) b;

	return strncmp(k1->afffile, k2->afffile, MAXPGPATH) ||
		strncmp(k1->dictfile, k2->dictfile, MAXPGPATH) ? 1 : 0;
}

/*
 * A hash function for TsearchDictKey.
 */
static uint32
dict_table_hash(const void *a, size_t size, void *arg)
{
	uint32		s;
	TsearchDictKey *k = (TsearchDictKey *) a;

	s = hash_combine(0, string_hash(k->afffile, MAXPGPATH));
	s = hash_combine(s, string_hash(k->dictfile, MAXPGPATH));

	return s;
}
