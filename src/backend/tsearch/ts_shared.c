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

#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "tsearch/ts_shared.h"


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

static HTAB *dict_table;

/*
 * Shared struct for locking
 */
typedef struct
{
	LWLock		lock;
} TsearchCtlData;

static TsearchCtlData *tsearch_ctl;

/*
 * GUC variable for maximum number of shared dictionaries
 */
int			shared_dictionaries = 10;

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
	void	   *res;

	StrNCpy(key.dictfile, dictfile, MAXPGPATH);
	StrNCpy(key.afffile, afffile, MAXPGPATH);

refind_entry:
	LWLockAcquire(&tsearch_ctl->lock, LW_SHARED);

	entry = (TsearchDictEntry *) hash_search(dict_table, &key, HASH_FIND,
											 &found);

	/* Dictionary wasn't load into memory */
	if (!found)
	{
		void	   *ispell_dict,
				   *dict_location;
		Size		ispell_size;

		/* Try to get exclusive lock */
		LWLockRelease(&tsearch_ctl->lock);
		if (!LWLockAcquireOrWait(&tsearch_ctl->lock, LW_EXCLUSIVE))
		{
			/*
			 * The lock was released by another backend, try to refind an entry.
			 */
			goto refind_entry;
		}

		entry = (TsearchDictEntry *) hash_search(dict_table, &key,
												 HASH_ENTER_NULL,
												 &found);

		/*
		 * There is no space in shared hash table, let backend to build the
		 * dictionary within its memory context.
		 */
		if (entry == NULL)
			return NULL;

		/* The lock was free so add new entry */
		ispell_dict = allocate_cb(dictbuild, dictfile, afffile, &ispell_size);

		seg = dsm_create(ispell_size, 0);
		dict_location = dsm_segment_address(seg);
		memcpy(dict_location, ispell_dict, ispell_size);

		pfree(ispell_dict);

		entry->dict_dsm = dsm_segment_handle(seg);

		/* Remain attached until end of postmaster */
		dsm_pin_segment(seg);
	}
	else
		seg = dsm_attach(entry->dict_dsm);

	LWLockRelease(&tsearch_ctl->lock);

	/* Remain attached until end of session */
	dsm_pin_mapping(seg);

	res = dsm_segment_address(seg);

	return res;
}

/*
 * Allocate and initialize tsearch-related shared memory.
 */
void
TsearchShmemInit(void)
{
	HASHCTL		ctl;
	bool		found;

	tsearch_ctl = (TsearchCtlData *)
		ShmemInitStruct("Full Text Search Ctl", sizeof(TsearchCtlData), &found);

	if (!found)
		LWLockInitialize(&tsearch_ctl->lock, LWTRANCHE_TSEARCH_DSA);

	memset(&ctl, 0, sizeof(ctl));
	ctl.keysize = sizeof(TsearchDictKey);
	ctl.entrysize = sizeof(TsearchDictEntry);

	dict_table = ShmemInitHash("Shared Tsearch Lookup Table",
							   shared_dictionaries, shared_dictionaries,
							   &ctl,
							   HASH_ELEM | HASH_BLOBS);
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
	size = add_size(size, hash_estimate_size(shared_dictionaries,
											 sizeof(TsearchDictEntry)));

	return size;
}
