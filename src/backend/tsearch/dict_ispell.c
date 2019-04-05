/*-------------------------------------------------------------------------
 *
 * dict_ispell.c
 *		Ispell dictionary interface
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 *
 * Compiled Ispell dictionaries are stored in DSM.  All necessary data are built
 * within dispell_build() function.  But structures for regular expressions are
 * compiled on first demand and stored using AffixReg array.  It is because
 * regex_t and Regis cannot be stored in shared memory easily.
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
	char	   *dict_name;
	StopList	stoplist;
	IspellDict	obj;
} DictISpell;

static void parse_dictoptions(List *dictoptions,
							  char **dictfile, char **afffile, char **stopfile);
static void *dispell_build(List *dictoptions, Size *size);

Datum
dispell_init(PG_FUNCTION_ARGS)
{
	DictInitData *init_data = (DictInitData *) PG_GETARG_POINTER(0);
	DictISpell *d;
	Size		dict_size;
	char	   *stopfile;

	d = (DictISpell *) palloc0(sizeof(DictISpell));

	parse_dictoptions(init_data->dict_options, NULL, NULL, &stopfile);

	if (stopfile)
		readstoplist(stopfile, &(d->stoplist), lowerstr);

	/*
	 * Build the dictionary in backend's memory if dictid is invalid (it may
	 * happen if the dicionary's init method was called within
	 * verify_dictoptions()).
	 */
	if (!OidIsValid(init_data->dict.id))
		d->obj.dict = dispell_build(init_data->dict_options, &dict_size);
	else
	{
		d->dict_name = ts_dict_shared_init(init_data, dispell_build);
		d->obj.dict = (IspellDictData *) ts_dict_shared_attach(d->dict_name,
															   &dict_size);
	}
	d->obj.reg = (AffixReg *) palloc0(d->obj.dict->nAffix * sizeof(AffixReg));

	/* Current memory context is dictionary's private memory context */
	d->obj.dictCtx = CurrentMemoryContext;

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

static void
parse_dictoptions(List *dictoptions, char **dictfile, char **afffile,
				  char **stopfile)
{
	ListCell   *l;

	if (dictfile)
		*dictfile = NULL;
	if (afffile)
		*afffile = NULL;
	if (stopfile)
		*stopfile = NULL;

	foreach(l, dictoptions)
	{
		DefElem    *defel = (DefElem *) lfirst(l);

		if (strcmp(defel->defname, "dictfile") == 0)
		{
			if (!dictfile)
				continue;

			if (*dictfile)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("multiple DictFile parameters")));
			*dictfile = get_tsearch_config_filename(defGetString(defel), "dict");
		}
		else if (strcmp(defel->defname, "afffile") == 0)
		{
			if (!afffile)
				continue;

			if (*afffile)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("multiple AffFile parameters")));
			*afffile = get_tsearch_config_filename(defGetString(defel), "affix");
		}
		else if (strcmp(defel->defname, "stopwords") == 0)
		{
			if (!stopfile)
				continue;

			if (*stopfile)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("multiple StopWords parameters")));
			*stopfile = defGetString(defel);
		}
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("unrecognized Ispell parameter: \"%s\"",
							defel->defname)));
		}
	}
}

/*
 * Build the dictionary.
 *
 * Result is palloc'ed.
 */
static void *
dispell_build(List *dictoptions, Size *size)
{
	IspellDictBuild build;
	char	   *dictfile,
			   *afffile;

	parse_dictoptions(dictoptions, &dictfile, &afffile, NULL);

	if (!afffile)
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("missing AffFile parameter")));
	}
	else if (!dictfile)
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("missing DictFile parameter")));
	}

	MemSet(&build, 0, sizeof(build));
	NIStartBuild(&build);

	/* Read files */
	NIImportDictionary(&build, dictfile);
	NIImportAffixes(&build, afffile);

	/* Build persistent data to use by backends */
	NISortDictionary(&build);
	NISortAffixes(&build);

	NICopyData(&build);

	/* Release temporary data */
	NIFinishBuild(&build);

	/* Return the buffer and its size */
	*size = build.dict_size;
	return build.dict;
}
