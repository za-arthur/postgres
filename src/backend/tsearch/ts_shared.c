/*-------------------------------------------------------------------------
 *
 * ts_shared.c
 *	  tsearch shared memory management
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
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

/* XXX should it be a GUC-variable? */
#define NUM_DICTIONARIES	20

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

typedef struct
{
	LWLock		lock;
} TsearchCtlData;

static TsearchCtlData *tsearch_ctl;
static HTAB *dict_table;

/*
 * Return handle to a dynamic shared memory.
 *
 * dictfile: .dict file of the dictionary.
 * afffile: .aff file of the dictionary.
 * allocate_cb: function to build the dictionary, if it wasn't found in DSM.
 */
dsm_handle
ispell_dsm_handle(const char *dictfile, const char *afffile,
				  ispell_build_callback allocate_cb)
{
	TsearchDictKey key;
	TsearchDictEntry *entry;
	bool		found;
	dsm_handle	res;

	StrNCpy(key.dictfile, dictfile, MAXPGPATH);
	StrNCpy(key.afffile, afffile, MAXPGPATH);

	LWLockAcquire(&tsearch_ctl->lock, LW_SHARED);

	entry = (TsearchDictEntry *) hash_search(dict_table, &key, HASH_FIND,
											 &found);

	/* Dictionary wasn't load into memory */
	if (!found)
	{
		/* Try to get exclusive lock */
		LWLockRelease(&tsearch_ctl->lock);
		if (!LWLockAcquireOrWait(&tsearch_ctl->lock, LW_EXCLUSIVE))
		{
			/*
			 * The lock was released by another backend, try to enter new
			 * TsearchDictEntry.
			 */
		}

		entry = (TsearchDictEntry *) hash_search(dict_table, &key, HASH_ENTER,
												 &found);
		if (found)
		{
			/* Other backend built the dictionary already */
			res = entry->dict_dsm;
		}
		else
		{
			const void *ispell_dict;
			Size		ispell_size;
			dsm_segment *seg;

			/* The lock was free so add new entry */
			ispell_dict = allocate_cb(dictfile, afffile, &ispell_size);

			seg = dsm_create(ispell_size, 0);
			memcpy(dsm_segment_address(seg), ispell_dict, ispell_size);

			entry->dict_dsm = dsm_segment_handle(seg);
			res = entry->dict_dsm;

			dsm_detach(seg);
		}
	}
	else
		res = entry->dict_dsm;

	LWLockRelease(&tsearch_ctl->lock);

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
		ShmemInitStruct("Full Text Search Ctl", TsearchShmemSize(), &found);

	if (!found)
		LWLockInitialize(&tsearch_ctl->lock, LWTRANCHE_TSEARCH_DSA);

	memset(&ctl, 0, sizeof(ctl));
	ctl.keysize = sizeof(TsearchDictKey);
	ctl.entrysize = sizeof(TsearchDictEntry);

	dict_table = ShmemInitHash("Shared Tsearch Lookup Table",
							   NUM_DICTIONARIES, NUM_DICTIONARIES,
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
	size = add_size(size, hash_estimate_size(NUM_DICTIONARIES,
											 sizeof(TsearchDictEntry)));

	return size;
}
