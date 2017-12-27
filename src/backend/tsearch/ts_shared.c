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
	dsa_handle	dsa_control;
	HTAB	   *dict_table;
	/* Concurently access to above fields */
	LWLock		lock;
} TsearchCtlData;

static TsearchCtlData *TsearchCtl;

static void *ispell_shmem_alloc(Size size);

/*
 * Return handle to a dynamic shared memory.
 *
 * dictfile: .dict file of the dictionary.
 * afffile: .aff file of the dictionary.
 */
dsm_handle
ispell_dsm_handle(const char *dictfile, const char *afffile)
{
	TsearchDictKey key;
	TsearchDictEntry *entry;
	bool		found;
	dsm_handle	res;

	LWLockAcquire(&TsearchCtl->lock, LW_EXCLUSIVE);

	if (TsearchCtl->dsa_control == DSM_HANDLE_INVALID)
	{
		dsa_area   *dsa = dsa_create(LWTRANCHE_TSEARCH_DSA);
		HASHCTL		ctl;
		char		tabname[MAXPGPATH * 2 + 16];

		TsearchCtl->dsa_control = dsa_get_handle(dsa);

		memset(&ctl, 0, sizeof(ctl));
		ctl.keysize = sizeof(TsearchDictKey);
		ctl.entrysize = sizeof(TsearchDictEntry);
		ctl.alloc = ispell_shmem_alloc;

		snprintf(tabname, sizeof(tabname), "ispell hash: %s, %s",
				 dictfile, afffile);
		TsearchCtl->dict_table = hash_create(tabname, 8, &ctl,
											 HASH_ELEM | HASH_ALLOC);
	}

	StrNCpy(key.dictfile, dictfile, MAXPGPATH);
	StrNCpy(key.afffile, afffile, MAXPGPATH);
	entry = (TsearchDictEntry *) hash_search(TsearchCtl->dict_table, &key,
											 HASH_ENTER, &found);
	res = entry->dict_dsm;

	LWLockRelease(&TsearchCtl->lock);

	return res;
}

/*
 * Allocate chunk from dynamic shared memory pointed by TsearchCtl->dsa_control.
 */
static void *
ispell_shmem_alloc(Size size)
{
	dsa_area   *dsa;
	dsa_pointer ptr;

	dsa = dsa_attach(TsearchCtl->dsa_control);
	ptr = dsa_allocate(dsa, size);

	return dsa_get_address(dsa, ptr);
}

/*
 * Allocate and initialize tsearch-related shared memory.
 */
void
TsearchShmemInit(void)
{
	bool		found;

	TsearchCtl = (TsearchCtlData *)
		ShmemInitStruct("Full Text Search Ctl", TsearchShmemSize(), &found);

	if (!found)
	{
		TsearchCtl->dsa_control = DSM_HANDLE_INVALID;
		TsearchCtl->dict_table = NULL;
		LWLockInitialize(&TsearchCtl->lock, LWTRANCHE_TSEARCH_DSA);
	}
}

/*
 * Report shared memory space needed by TsearchShmemInit.
 */
Size
TsearchShmemSize(void)
{
	Size		size = 0;

	size = add_size(size, sizeof(TsearchCtlData));

	return size;
}
