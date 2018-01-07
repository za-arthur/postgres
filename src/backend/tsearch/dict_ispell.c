/*-------------------------------------------------------------------------
 *
 * dict_ispell.c
 *		Ispell dictionary interface
 *
 * By default all Ispell dictionaries are stored in DSM. But if number of
 * loaded dictionaries reached maximum allowed value then it will be
 * allocated within its memory context (dictCtx).
 *
 * All necessary data are built within dispell_build() function. But
 * structures for regular expressions are compiled on first demand and
 * stored using AffixReg array. It is because regex_t and Regis cannot be
 * stored in shared memory.
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/tsearch/dict_ispell.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "commands/defrem.h"
#include "storage/dsm.h"
#include "tsearch/dicts/spell.h"
#include "tsearch/ts_locale.h"
#include "tsearch/ts_shared.h"
#include "tsearch/ts_utils.h"
#include "utils/builtins.h"


typedef struct
{
	StopList	stoplist;
	IspellDictBuild build;
	IspellDict	obj;
	dsm_handle	dict_handle;
} DictISpell;

static void *dispell_build(void *dictbuild,
						   const char *dictfile, const char *afffile,
						   Size *size);

Datum
dispell_init(PG_FUNCTION_ARGS)
{
	List	   *dictoptions = (List *) PG_GETARG_POINTER(0);
	DictISpell *d;
	char	   *dictfile = NULL,
			   *afffile = NULL;
	bool		stoploaded = false;
	ListCell   *l;

	d = (DictISpell *) palloc0(sizeof(DictISpell));

	foreach(l, dictoptions)
	{
		DefElem    *defel = (DefElem *) lfirst(l);

		if (pg_strcasecmp(defel->defname, "DictFile") == 0)
		{
			if (dictfile)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("multiple DictFile parameters")));
			dictfile = get_tsearch_config_filename(defGetString(defel), "dict");
		}
		else if (pg_strcasecmp(defel->defname, "AffFile") == 0)
		{
			if (afffile)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("multiple AffFile parameters")));
			afffile = get_tsearch_config_filename(defGetString(defel), "affix");
		}
		else if (pg_strcasecmp(defel->defname, "StopWords") == 0)
		{
			if (stoploaded)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("multiple StopWords parameters")));
			readstoplist(defGetString(defel), &(d->stoplist), lowerstr);
			stoploaded = true;
		}
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("unrecognized Ispell parameter: \"%s\"",
							defel->defname)));
		}
	}

	if (dictfile && afffile)
	{
		dsm_segment *seg;
		uint32		naffix;

		d->dict_handle = ispell_shmem_location(&(d->build), dictfile, afffile,
											   dispell_build);

		/*
		 * There is no space in shared memory, build the dictionary within its
		 * memory context.
		 */
		if (d->dict_handle == DSM_HANDLE_INVALID)
		{
			Size		ispell_size;

			d->obj.dict = (IspellDictData *) dispell_build(&(d->build),
														   dictfile, afffile,
														   &ispell_size);
			naffix = d->obj.dict->nAffix;
		}
		/* The dictionary was allocated in DSM */
		else
		{
			IspellDictData *dict;

			seg = dsm_attach(d->dict_handle);
			dict = (IspellDictData *) dsm_segment_address(seg);

			/* We need to save naffix here because seg will be detached */
			naffix = dict->nAffix;

			dsm_detach(seg);
		}

		d->obj.reg = (AffixReg *) palloc0(naffix * sizeof(AffixReg));
		/* Current memory context is dictionary's private memory context */
		d->obj.dictCtx = CurrentMemoryContext;
	}
	else if (!afffile)
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("missing AffFile parameter")));
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("missing DictFile parameter")));
	}

	PG_RETURN_POINTER(d);
}

Datum
dispell_lexize(PG_FUNCTION_ARGS)
{
	DictISpell *d = (DictISpell *) PG_GETARG_POINTER(0);
	char	   *in = (char *) PG_GETARG_POINTER(1);
	int32		len = PG_GETARG_INT32(2);
	dsm_segment *seg = NULL;
	char	   *txt;
	TSLexeme   *res;
	TSLexeme   *ptr,
			   *cptr;

	if (len <= 0)
		PG_RETURN_POINTER(NULL);

	/*
	 * If the dictionary allocated in DSM, get a pointer to IspellDictData.
	 * Otherwise d->obj.dict already points to IspellDictData allocated within
	 * the dictionary's memory context.
	 */
	if (d->dict_handle != DSM_HANDLE_INVALID)
	{
		seg = dsm_attach(d->dict_handle);
		d->obj.dict = (IspellDictData *) dsm_segment_address(seg);
	}

	txt = lowerstr_with_len(in, len);
	res = NINormalizeWord(&(d->obj), txt);

	if (res == NULL)
	{
		if (seg)
			dsm_detach(seg);
		PG_RETURN_POINTER(NULL);
	}

	cptr = res;
	for (ptr = cptr; ptr->lexeme; ptr++)
	{
		if (searchstoplist(&(d->stoplist), ptr->lexeme))
		{
			pfree(ptr->lexeme);
			ptr->lexeme = NULL;
		}
		else
		{
			if (cptr != ptr)
				memcpy(cptr, ptr, sizeof(TSLexeme));
			cptr++;
		}
	}
	cptr->lexeme = NULL;

	if (seg)
		dsm_detach(seg);
	PG_RETURN_POINTER(res);
}

/*
 * Build the dictionary.
 *
 * Result is palloc'ed.
 */
static void *
dispell_build(void *dictbuild, const char *dictfile, const char *afffile,
			  Size *size)
{
	IspellDictBuild *build = (IspellDictBuild *) dictbuild;

	Assert(dictfile && afffile);

	NIStartBuild(build);

	/* Read files */
	NIImportDictionary(build, dictfile);
	NIImportAffixes(build, afffile);

	/* Build persistent data to use by backends */
	NISortDictionary(build);
	NISortAffixes(build);

	NICopyData(build);

	/* Release temporary data */
	NIFinishBuild(build);

	/* Return the buffer and its size */
	*size = build->dict_size;
	return build->dict;
}
