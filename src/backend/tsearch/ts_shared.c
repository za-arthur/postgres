/*-------------------------------------------------------------------------
 *
 * ts_shared.c
 *	  Text search shared dictionary management
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/tsearch/ts_shared.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>
#ifndef WIN32
#include <sys/mman.h>
#endif
#include <sys/stat.h>

#include "pgstat.h"
#include "storage/fd.h"
#include "tsearch/ts_shared.h"


char *
ts_dict_shared_init(DictInitData *init_data, ts_dict_build_callback allocate_cb)
{
	char	   *name;
	int			flags;
	int			fd;
	void	   *dict;
	Size		dict_size;

	/*
	 * Build the dictionary in backend's memory if dictid is invalid (it may
	 * happen if the dicionary's init method was called within
	 * verify_dictoptions()).
	 */
	if (!OidIsValid(init_data->dict.id))
	{
		dict = allocate_cb(init_data->dict_options, &dict_size);

		return dict;
	}

	name = psprintf(PG_SHDICT_DIR "/%u", init_data->dict.id);

	/* Try to create a new file */
	flags = O_RDWR | O_CREAT | O_EXCL | PG_BINARY;
	if ((fd = OpenTransientFile(name, flags)) == -1)
	{
		if (errno != EEXIST)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not open shared dictionary file \"%s\": %m",
							name)));
		/* The file was created before */
		return name;
	}

	/* Build the dictionary */
	dict = allocate_cb(init_data->dict_options, &dict_size);

	/* And write it to the shared file */
	pgstat_report_wait_start(WAIT_EVENT_TS_SHARED_DICT_WRITE);
	if (write(fd, dict, dict_size) != dict_size)
	{
		pgstat_report_wait_end();
		/* if write didn't set errno, assume problem is no disk space */
		if (errno == 0)
			errno = ENOSPC;
		CloseTransientFile(fd);

		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write to shared dictionary file \"%s\": %m",
						name)));
	}
	pgstat_report_wait_end();

	pfree(dict);

	if (CloseTransientFile(fd))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close shared dictionary file \"%s\": %m",
						name)));

	return name;
}

void *
ts_dict_shared_attach(const char *dict_name, Size *dict_size)
{
	int			flags;
	int			fd;
	void	   *address;
	struct stat st;

	/* Open an existing file for attach */
	flags = O_RDONLY | PG_BINARY;
	if ((fd = OpenTransientFile(dict_name, flags)) == -1)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open shared dictionary file \"%s\": %m",
						dict_name)));

	if (fstat(fd, &st) != 0)
	{
		int			save_errno;

		save_errno = errno;
		CloseTransientFile(fd);
		errno = save_errno;

		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not stat shared dictionary file \"%s\": %m",
						dict_name)));
	}
	*dict_size = st.st_size;

	/* Map the shared file. We need only read access */
	address = mmap(NULL, *dict_size, PROT_READ, MAP_SHARED, fd, 0);
	if (address == MAP_FAILED)
	{
		int			save_errno;

		save_errno = errno;
		CloseTransientFile(fd);
		errno = save_errno;

		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not map shared dictionary file \"%s\": %m",
						dict_name)));
		return false;
	}

	if (CloseTransientFile(fd))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close shared dictionary file \"%s\": %m",
						dict_name)));

	return address;
}

void
ts_dict_shared_detach(const char *dict_name, void *dict_address, Size dict_size)
{
	if (munmap(dict_address, dict_size) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not unmap shared memory segment \"%s\": %m",
						dict_name)));
}
