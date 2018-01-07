/*-------------------------------------------------------------------------
 *
 * ts_shared.h
 *	  tsearch shared memory management
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/tsearch/ts_shared.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef TS_SHARED_H
#define TS_SHARED_H

#include "c.h"
#include "storage/dsm.h"

/*
 * GUC variable for maximum number of shared dictionaries
 */
extern int shared_dictionaries;

typedef void *(*ispell_build_callback) (void *dictbuild,
										const char *dictfile,
										const char *afffile,
										Size *size);

extern dsm_handle ispell_shmem_location(void *dictbuild,
									  const char *dictfile, const char *afffile,
									  ispell_build_callback allocate_cb);

extern void TsearchShmemInit(void);
extern Size TsearchShmemSize(void);

#endif							/* TS_SHARED_H */
