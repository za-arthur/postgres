/*-------------------------------------------------------------------------
 *
 * ts_shared.h
 *	  tsearch shared memory management
 *
 * Portions Copyright (c) 1996-2018, PostgreSQL Global Development Group
 *
 * src/include/tsearch/ts_shared.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef TS_SHARED_H
#define TS_SHARED_H

#include "c.h"

/*
 * GUC variable for maximum number of shared dictionaries
 */
extern int max_shared_dictionaries_size;

typedef void *(*ispell_build_callback) (void *arg, Size *size);

extern void *ts_dict_shmem_location(Oid dictid, void *arg,
									ispell_build_callback allocate_cb);

extern void TsearchShmemInit(void);
extern Size TsearchShmemSize(void);

#endif							/* TS_SHARED_H */
