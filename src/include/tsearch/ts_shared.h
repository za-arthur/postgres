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

#include "tsearch/ts_public.h"

/*
 * Value for max_shared_dictionaries_size, means that there is no limit in
 * shared memory for shared dictionaries.
 */
#define TS_DICT_SHMEM_UNLIMITED		(-1)

/*
 * GUC variable for maximum number of shared dictionaries
 */
extern int max_shared_dictionaries_size;

typedef void *(*ts_dict_build_callback) (List *dictoptions, Size *size);

extern void *ts_dict_shmem_location(DictInitData *initoptions,
									ts_dict_build_callback allocate_cb);
extern void ts_dict_shmem_release(Oid dictid);

extern void TsearchShmemInit(void);
extern Size TsearchShmemSize(void);

#endif							/* TS_SHARED_H */
