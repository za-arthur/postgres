/*-------------------------------------------------------------------------
 *
 * ts_shared.h
 *	  manage sharing tsearch dictionaries between backends
 *
 * Portions Copyright (c) 1996-2018, PostgreSQL Global Development Group
 *
 * src/include/tsearch/ts_shared.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef TS_SHARED_H
#define TS_SHARED_H

#define PG_TSCACHE_DIR					"pg_tscache"
#define PG_TSCACHE_MMAP_FILE_PREFIX		"mmap."

typedef void *(*ts_dict_build_callback) (List *dictoptions, Size *size);

extern void *ts_dict_shared_location(Oid dictid, List *dictoptions,
									 ts_dict_build_callback allocate_cb);
extern void ts_dict_shared_release(Oid dictid);

extern void TsearchShmemInit(void);
extern Size TsearchShmemSize(void);

#endif							/* TS_SHARED_H */
