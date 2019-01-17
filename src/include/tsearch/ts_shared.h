/*-------------------------------------------------------------------------
 *
 * ts_shared.h
 *	  Text search shared dictionary management
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 *
 * src/include/tsearch/ts_shared.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef TS_SHARED_H
#define TS_SHARED_H

#include "tsearch/ts_public.h"

#define PG_SHDICT_DIR					"pg_shdict"

typedef void *(*ts_dict_build_callback) (List *dictoptions, Size *size);

extern char *ts_dict_shared_init(DictInitData *init_data,
								 ts_dict_build_callback allocate_cb);
extern void *ts_dict_shared_attach(const char *dict_name, Size *dict_size);
extern void ts_dict_shared_detach(const char *dict_name, void *dict_address,
								  Size dict_size);

#endif							/* TS_SHARED_H */
