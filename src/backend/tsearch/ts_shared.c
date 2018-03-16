/*-------------------------------------------------------------------------
 *
 * ts_shared.c
 *	  manage sharing tsearch dictionaries between backends
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

#include <fcntl.h>
#include <unistd.h>
#ifndef WIN32
#include <sys/mman.h>
#endif
#include <sys/stat.h>

#include "lib/dshash.h"
#include "nodes/pg_list.h"
#include "pgstat.h"
#include "portability/mem.h"
#include "storage/fd.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "tsearch/ts_shared.h"
#include "utils/memutils.h"

/*
 * Content of a mapped file into memory.
 */
typedef struct
{
	/* PostgreSQL major version number to check mapped file */
	uint32		pg_major_version;
	/* Content of a dictionary */
	char		dict[FLEXIBLE_ARRAY_MEMBER];
} TsearchSharedHeader;

/*
 * Hash table structures
 */
typedef struct
{
	Oid			dict_id;
	Size		dict_size;

	/* How many backends share the dictionary */
	uint32		refcnt;
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

static void init_dict_table(void);

/* Parameters for dict_table */
static const dshash_parameters dict_table_params = {
	sizeof(Oid),
	sizeof(TsearchDictEntry),
	dshash_memcmp,
	dshash_memhash,
	LWTRANCHE_TSEARCH_TABLE
};

/* Size of buffer to be used for zero-filling. */
#define ZBUFFER_SIZE				8192

static void *get_dict_address(const char *file_name, Size request_size,
							  bool create_file);
static void release_dict_cache(Oid dictid);
static int errcode_for_shared_tsearch(void);

/*
 * Build the dictionary using allocate_cb callback.
 *
 * dictid: Oid of the dictionary.
 * dictoptions: options used to build the dictionary.
 * allocate_cb: function to build the dictionary, if it wasn't found in DSM.
 *
 * Returns address in the dynamic shared memory segment or in backend memory.
 */
void *
ts_dict_shared_location(Oid dictid, List *dictoptions,
						ts_dict_build_callback allocate_cb)
{
	TsearchDictEntry *entry;
	bool		found;
	char		file_name[64];
	void	   *address;
	struct stat st;
	bool		exists = false;

	init_dict_table();

	/*
	 * Build the dictionary in backend's memory if dictid is invalid. It may
	 * happen if the dicionary's init method was called within
	 * verify_dictoptions().
	 */
	if (!OidIsValid(dictid))
	{
		void	   *dict;
		Size		dict_size;

		dict = allocate_cb(dictoptions, &dict_size);

		return dict;
	}

	snprintf(file_name, 64, PG_TSCACHE_DIR "/" PG_TSCACHE_MMAP_FILE_PREFIX "%u",
			 dictid);

	/* Try to find an entry in the hash table */
	entry = (TsearchDictEntry *) dshash_find(dict_table, &dictid, false);

	if (entry)
	{
		address = get_dict_address(file_name, entry->dict_size, false);

		entry->refcnt++;
		dshash_release_lock(dict_table, entry);

		return address;
	}

	/* Dictionary haven't been mapped into memory yet */
	entry = (TsearchDictEntry *) dshash_find_or_insert(dict_table, &dictid,
													   &found);

	if (found)
	{
		/*
		 * Someone concurrently inserted a dictionary entry since the first time
		 * we checked.
		 */
		address = get_dict_address(file_name, entry->dict_size, false);

		entry->refcnt++;
		dshash_release_lock(dict_table, entry);

		return address;
	}

	/*
	 * Before building the dictionary check for existance of the cache file.
	 */
	if (stat(file_name, &st))
	{
		if (errno != ENOENT)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not stat text search cache file \"%s\": %m",
							file_name)));
	}
	else
		exists = true;

	/* Target file exists, try to map it into memory */
	if (exists)
	{
		entry->dict_size = st.st_size;
		address = get_dict_address(file_name, st.st_size, false);
	}
	/* Target file doesn't exist, so try to create it and map it into memory */
	else
	{
		void	   *dict;

		/* Firstly build the dictionary */
		dict = allocate_cb(dictoptions, &entry->dict_size);
		/* Then create the file and map it */
		address = get_dict_address(file_name, entry->dict_size, true);
		memcpy(address, dict, entry->dict_size);

		pfree(dict);
	}

	entry->dict_id = dictid;
	entry->refcnt = 1;

	dshash_release_lock(dict_table, entry);

	return address;
}

/*
 * Release space occupied by the dictionary.
 *
 * dictid: Oid of the dictionary.
 */
void
ts_dict_shared_release(Oid dictid)
{
	bool		removed = false;

	/* if removed the dictionary */
	/* if removed the database */
	/* other cases? */

	/* Firstly remove an entry from the hash table */
	if (dict_table)
	{
		TsearchDictEntry *entry;

		/* Try to find an entry in the hash table */
		entry = (TsearchDictEntry *) dshash_find(dict_table, &dictid, true);

		if (entry)
		{
			entry->refcnt--;

			if (entry->refcnt == 0)
			{
				/* It is safe to remove cache file now */
				release_dict_cache(entry->dict_id);
				removed = true;

				dshash_delete_entry(dict_table, entry);
			}
			else
			{
				dshash_release_lock(dict_table, entry);

				/*
				 * We don't want to remove cache file now because there are
				 * backends which point to the shared location.
				 */
				return;
			}
		}
	}

	/*
	 * Cache file wasn't removed because there is no corresponding entry
	 * within hash table. It is safe to remove it now, there are no backends
	 * which point to the shared location.
	 */
	if (!removed)
		release_dict_cache(dictid);
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
 * max_shared_dictionaries_size GUC is greater than zero and it doesn't exist
 * yet.
 */
static void
init_dict_table(void)
{
	MemoryContext old_context;
	dsa_area   *dsa;

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

#ifndef WIN32
/*
 * Map file with a dictionary content into memory using mmap().
 */
static void *
get_dict_address(const char *file_name, Size request_size, bool create_file)
{
	int			flags;
	int			fd;
	void	   *address;
	TsearchSharedHeader *dict;

	flags = O_RDWR | (create_file ? O_CREAT | O_EXCL : 0);
	if ((fd = OpenTransientFile(file_name, flags)) == -1)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open text search cache file \"%s\": %m",
						file_name)));

	if (create_file)
	{
		/*
		 * Allocate a buffer full of zeros.
		 *
		 * Note: palloc zbuffer, instead of just using a local char array, to
		 * ensure it is reasonably well-aligned; this may save a few cycles
		 * transferring data to the kernel.
		 */
		char	   *zbuffer = (char *) palloc0(ZBUFFER_SIZE);
		uint32		remaining = request_size;
		bool		success = true;

		/*
		 * Zero-fill the file. We have to do this the hard way to ensure that
		 * all the file space has really been allocated, so that we don't
		 * later seg fault when accessing the memory mapping.  This is pretty
		 * pessimal.
		 */
		while (success && remaining > 0)
		{
			Size		goal = remaining;

			if (goal > ZBUFFER_SIZE)
				goal = ZBUFFER_SIZE;
			pgstat_report_wait_start(WAIT_EVENT_TS_SHARED_FILL_ZERO_WRITE);
			if (write(fd, zbuffer, goal) == goal)
				remaining -= goal;
			else
				success = false;
			pgstat_report_wait_end();
		}

		if (!success)
		{
			int			save_errno;

			/* Back out what's already been done. */
			save_errno = errno;
			CloseTransientFile(fd);
			if (create_file)
				unlink(file_name);
			errno = save_errno ? save_errno : ENOSPC;

			ereport(ERROR,
					(errcode_for_shared_tsearch(),
					 errmsg("could not resize text search cache file \"%s\" to %zu bytes: %m",
							file_name, request_size)));
		}
	}

	address = mmap(NULL, request_size, PROT_READ | PROT_WRITE,
				   MAP_SHARED | MAP_HASSEMAPHORE | MAP_NOSYNC, fd, 0);
	if (address == MAP_FAILED)
	{
		int			save_errno;

		save_errno = errno;
		CloseTransientFile(fd);
		if (create_file)
			unlink(file_name);
		errno = save_errno;

		ereport(ERROR,
				(errcode_for_shared_tsearch(),
				 errmsg("could not map text search cache file \"%s\": %m",
						file_name)));
	}

	CloseTransientFile(fd);

	dict = (TsearchSharedHeader *) address;
	if (create_file)
		dict->pg_major_version = PG_VERSION_NUM / 100;
	else if (dict->pg_major_version != PG_VERSION_NUM / 100)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("incompatible text search cache file version")));

	return (void *) dict->dict;
}

/*
 * Check for existance of the dictionary cache file.  If it exists unmap and
 * remove it.
 */
static void
release_dict_cache(Oid dictid)
{
	char		file_name[64];

	snprintf(file_name, 64, PG_TSCACHE_DIR "/" PG_TSCACHE_MMAP_FILE_PREFIX "%u",
			 dictid);

	/*
	 * Check for existance of the dictionary cache file.
	 */
	if (stat(file_name, &st))
	{
		if (errno != ENOENT)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not stat text search cache file \"%s\": %m",
							file_name)));
		else
		{
			/* The file doesn't exist */
			return;
		}
	}


}
#endif

static int
errcode_for_shared_tsearch(void)
{
	if (errno == EFBIG || errno == ENOMEM)
		return errcode(ERRCODE_OUT_OF_MEMORY);
	else
		return errcode_for_file_access();
}
