/*-------------------------------------------------------------------------
 *
 * dict_ispell.c
 *		Ispell dictionary interface
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
#include "tsearch/dicts/spell.h"
#include "tsearch/ts_locale.h"
#include "tsearch/ts_shared.h"
#include "tsearch/ts_utils.h"
#include "utils/builtins.h"


typedef struct
{
	StopList	stoplist;
	IspellDictBuild build;
} DictISpell;

/*
 * Build the dictionary.
 *
 * Result is palloc'ed.
 */
static const void *
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

	/* Release temporary data */
	NIFinishBuild(build);

	/* Return the buffer and its size */
	*size = build->dict_size;
	return build->dict;
}

Datum
dispell_init(PG_FUNCTION_ARGS)
{
	List	   *dictoptions = (List *) PG_GETARG_POINTER(0);
	DictISpell *d;
	const	   *dictfile = NULL,
			   *afffile = NULL;
	bool		stoploaded = false;
	ListCell   *l;

	d = (DictISpell *) palloc0(sizeof(DictISpell));

	NIStartBuild(&(d->build));
	ispell_shmem_location("test1","test2", dispell_build);

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

	if (dictfile && affile)
	{
		NISortDictionary(&(d->build));
		NISortAffixes(&(d->build));
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

	NIFinishBuild(&(d->build));

	PG_RETURN_POINTER(d);
}

Datum
dispell_lexize(PG_FUNCTION_ARGS)
{
	DictISpell *d = (DictISpell *) PG_GETARG_POINTER(0);
	char	   *in = (char *) PG_GETARG_POINTER(1);
	int32		len = PG_GETARG_INT32(2);
	char	   *txt;
	TSLexeme   *res;
	TSLexeme   *ptr,
			   *cptr;

	if (len <= 0)
		PG_RETURN_POINTER(NULL);

	txt = lowerstr_with_len(in, len);
	res = NINormalizeWord(&(d->obj), txt);

	if (res == NULL)
		PG_RETURN_POINTER(NULL);

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

	PG_RETURN_POINTER(res);
}
