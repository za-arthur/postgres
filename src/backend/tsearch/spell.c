/*-------------------------------------------------------------------------
 *
 * spell.c
 *		Normalizing word with ISpell
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 *
 * Ispell dictionary
 * -----------------
 *
 * Rules of dictionaries are defined in two files with .affix and .dict
 * extensions. They are used by spell checker programs Ispell and Hunspell.
 *
 * An .affix file declares morphological rules to get a basic form of words.
 * The format of an .affix file has different structure for Ispell and Hunspell
 * dictionaries. The Hunspell format is more complicated. But when an .affix
 * file is imported and compiled, it is stored in the same structure AffixNode.
 *
 * A .dict file stores a list of basic forms of words with references to
 * affix rules. The format of a .dict file has the same structure for Ispell
 * and Hunspell dictionaries.
 *
 * Compilation of a dictionary
 * ---------------------------
 *
 * A compiled dictionary is stored in the IspellDict structure. Compilation of
 * a dictionary is divided into the several steps:
 *	- NIImportDictionary() - stores each word of a .dict file in the
 *	  temporary Spell field.
 *	- NIImportAffixes() - stores affix rules of an .affix file in the
 *	  Affix field (not temporary) if an .affix file has the Ispell format.
 *	  -> NIImportOOAffixes() - stores affix rules if an .affix file has the
 *		 Hunspell format. The AffixData field is initialized if AF parameter
 *		 is defined.
 *	- NISortDictionary() - builds a prefix tree (Trie) from the words list
 *	  and stores it in the Dictionary field. The words list is got from the
 *	  Spell field. The AffixData field is initialized if AF parameter is not
 *	  defined.
 *	- NISortAffixes():
 *	  - builds a list of compound affixes from the affix list and stores it
 *		in the CompoundAffix.
 *	  - builds prefix trees (Trie) from the affix list for prefixes and suffixes
 *		and stores them in Suffix and Prefix fields.
 *	  The affix list is got from the Affix field.
 *
 * Memory management
 * -----------------
 *
 * The IspellDictBuild structure has the Spell field which is used only in
 * compile time. The Spell field stores a words list. It can take a lot of
 * memory. Therefore when a dictionary is compiled this field is cleared by
 * NIFinishBuild().
 *
 * All resources which should cleared by NIFinishBuild() is initialized using
 * tmpalloc() and tmpalloc0().
 *
 * IDENTIFICATION
 *	  src/backend/tsearch/spell.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "catalog/pg_collation.h"
#include "tsearch/dicts/spell.h"
#include "tsearch/ts_locale.h"
#include "utils/memutils.h"


/*
 * Initialization requires a lot of memory that's not needed
 * after the initialization is done.  During initialization,
 * CurrentMemoryContext is the long-lived memory context associated
 * with the dictionary cache entry.  We keep the short-lived stuff
 * in the ConfBuild->buildCxt context.
 */
#define tmpalloc(sz)  MemoryContextAlloc(ConfBuild->buildCxt, (sz))

/*
 * Prepare for constructing an ISpell dictionary.
 *
 * The IspellDictBuild struct is assumed to be zeroed when allocated.
 */
void
NIStartBuild(IspellDictBuild *ConfBuild)
{
	uint32		dict_size;

	/*
	 * The temp context is a child of CurTransactionContext, so that it will
	 * go away automatically on error.
	 */
	ConfBuild->buildCxt = AllocSetContextCreate(CurTransactionContext,
											   "Ispell dictionary init context",
												ALLOCSET_DEFAULT_SIZES);

	/*
	 * Allocate buffer for the dictionary in current context not in buildCxt.
	 * Initially allocate 2MB for IspellDictData.
	 */
	dict_size = IspellDictDataHdrSize + 2 * 1024 * 1024;
	ConfBuild->dict = palloc(dict_size);
	ConfBuild->dict_size = dict_size;
}

/*
 * Clean up when dictionary construction is complete.
 */
void
NIFinishBuild(IspellDictBuild *ConfBuild)
{
	/* Release no-longer-needed temp memory */
	MemoryContextDelete(ConfBuild->buildCxt);
	/* Just for cleanliness, zero the now-dangling pointers */
	ConfBuild->buildCxt = NULL;
	ConfBuild->Spell = NULL;
	ConfBuild->CompoundAffixFlags = NULL;
}


/*
 * "Compact" palloc: allocate without extra palloc overhead.
 *
 * Since we have no need to free the ispell data items individually, there's
 * not much value in the per-chunk overhead normally consumed by palloc.
 * Getting rid of it is helpful since ispell can allocate a lot of small nodes.
 *
 * We currently pre-zero all data allocated this way, even though some of it
 * doesn't need that.  The cpalloc and cpalloc0 macros are just documentation
 * to indicate which allocations actually require zeroing.
 */
#define COMPACT_ALLOC_CHUNK 8192	/* amount to get from palloc at once */
#define COMPACT_MAX_REQ		1024	/* must be < COMPACT_ALLOC_CHUNK */

static void *
compact_palloc0(IspellDict *Conf, size_t size)
{
	void	   *result;

	/* No point in this for large chunks */
	if (size > COMPACT_MAX_REQ)
		return palloc0(size);

	/* Keep everything maxaligned */
	size = MAXALIGN(size);

	/* Need more space? */
	if (size > Conf->avail)
	{
		Conf->firstfree = palloc0(COMPACT_ALLOC_CHUNK);
		Conf->avail = COMPACT_ALLOC_CHUNK;
	}

	result = (void *) Conf->firstfree;
	Conf->firstfree += size;
	Conf->avail -= size;

	return result;
}

#define cpalloc(size) compact_palloc0(Conf, size)
#define cpalloc0(size) compact_palloc0(Conf, size)

static char *
cpstrdup(IspellDict *Conf, const char *str)
{
	char	   *res = cpalloc(strlen(str) + 1);

	strcpy(res, str);
	return res;
}


/*
 * Apply lowerstr(), producing a temporary result (in the buildCxt).
 */
static char *
lowerstr_ctx(IspellDictBuild *ConfBuild, const char *src)
{
	MemoryContext saveCtx;
	char	   *dst;

	saveCtx = MemoryContextSwitchTo(ConfBuild->buildCxt);
	dst = lowerstr(src);
	MemoryContextSwitchTo(saveCtx);

	return dst;
}

#define MAX_NORM 1024
#define MAXNORMLEN 256

#define STRNCMP(s,p)	strncmp( (s), (p), strlen(p) )
#define GETWCHAR(W,L,N,T) ( ((const uint8*)(W))[ ((T)==FF_PREFIX) ? (N) : ( (L) - 1 - (N) ) ] )
#define GETCHAR(A,N,T)	  GETWCHAR( AffixFieldRepl(A), (A)->replen, N, T )

static char *VoidString = "";

static int
cmpspell(const void *s1, const void *s2)
{
	return strcmp((*(SPELL *const *) s1)->word, (*(SPELL *const *) s2)->word);
}

static int
cmpspellaffix(const void *s1, const void *s2)
{
	return strcmp((*(SPELL *const *) s1)->p.flag,
				  (*(SPELL *const *) s2)->p.flag);
}

static int
cmpcmdflag(const void *f1, const void *f2)
{
	CompoundAffixFlag *fv1 = (CompoundAffixFlag *) f1,
			   *fv2 = (CompoundAffixFlag *) f2;

	Assert(fv1->flagMode == fv2->flagMode);

	if (fv1->flagMode == FM_NUM)
	{
		if (fv1->flag.i == fv2->flag.i)
			return 0;

		return (fv1->flag.i > fv2->flag.i) ? 1 : -1;
	}

	return strcmp(fv1->flag.s, fv2->flag.s);
}

static char *
findchar(char *str, int c)
{
	while (*str)
	{
		if (t_iseq(str, c))
			return str;
		str += pg_mblen(str);
	}

	return NULL;
}

static char *
findchar2(char *str, int c1, int c2)
{
	while (*str)
	{
		if (t_iseq(str, c1) || t_iseq(str, c2))
			return str;
		str += pg_mblen(str);
	}

	return NULL;
}


/* backward string compare for suffix tree operations */
static int
strbcmp(const unsigned char *s1, const unsigned char *s2)
{
	int			l1 = strlen((const char *) s1) - 1,
				l2 = strlen((const char *) s2) - 1;

	while (l1 >= 0 && l2 >= 0)
	{
		if (s1[l1] < s2[l2])
			return -1;
		if (s1[l1] > s2[l2])
			return 1;
		l1--;
		l2--;
	}
	if (l1 < l2)
		return -1;
	if (l1 > l2)
		return 1;

	return 0;
}

static int
strbncmp(const unsigned char *s1, const unsigned char *s2, size_t count)
{
	int			l1 = strlen((const char *) s1) - 1,
				l2 = strlen((const char *) s2) - 1,
				l = count;

	while (l1 >= 0 && l2 >= 0 && l > 0)
	{
		if (s1[l1] < s2[l2])
			return -1;
		if (s1[l1] > s2[l2])
			return 1;
		l1--;
		l2--;
		l--;
	}
	if (l == 0)
		return 0;
	if (l1 < l2)
		return -1;
	if (l1 > l2)
		return 1;
	return 0;
}

/*
 * Compares affixes.
 * First compares the type of an affix. Prefixes should go before affixes.
 * If types are equal then compares replaceable string.
 */
static int
cmpaffix(const void *s1, const void *s2)
{
	const AFFIX *a1 = *((AFFIX *const *) s1);
	const AFFIX *a2 = *((AFFIX *const *) s2);

	if (a1->type < a2->type)
		return -1;
	if (a1->type > a2->type)
		return 1;
	if (a1->type == FF_PREFIX)
		return strcmp(AffixFieldRepl(a1), AffixFieldRepl(a2));
	else
		return strbcmp((const unsigned char *) AffixFieldRepl(a1),
					   (const unsigned char *) AffixFieldRepl(a2));
}

/*
 * Allocate space for AffixData.
 */
static void
NIInitAffixData(IspellDictBuild *ConfBuild, int numAffixData)
{
	uint32		size;

	size = 8 * 1024 /* Reserve 8KB for data */;

	ConfBuild->AffixData = (char *) tmpalloc(size);
	ConfBuild->AffixDataSize = size;
	ConfBuild->AffixDataOffset = (uint32 *) tmpalloc(numAffixData * sizeof(uint32));
	ConfBuild->nAffixData = 0;
	ConfBuild->mAffixData= numAffixData;

	/* Save offset of the end of data */
	ConfBuild->AffixDataEnd = 0;
}

/*
 * Add affix set of affix flags into IspellDict struct.  If IspellDict doesn't
 * fit new affix set then resize it.
 *
 * ConfBuild: building structure for the current dictionary.
 * AffixSet: set of affix flags.
 */
static void
NIAddAffixSet(IspellDictBuild *ConfBuild, const char *AffixSet,
			  uint32 AffixSetLen)
{
	/*
	 * Check available space for AffixSet.
	 */
	if (ConfBuild->AffixDataEnd + AffixSetLen + 1 /* \0 */ >=
		ConfBuild->AffixDataSize)
	{
		uint32		newsize = Max(ConfBuild->AffixDataSize + 8 * 1024 /* 8KB */,
								  ConfBuild->AffixDataSize + AffixSetLen + 1);

		ConfBuild->AffixData = (char *) repalloc(ConfBuild->AffixData, newsize);
		ConfBuild->AffixDataSize = newsize;
	}

	/* Check available number of offsets */
	if (ConfBuild->nAffixData >= ConfBuild->mAffixData)
	{
		ConfBuild->mAffixData *= 2;
		ConfBuild->AffixDataOffset = (uint32 *) repalloc(ConfBuild->AffixDataOffset,
														 sizeof(uint32) * ConfBuild->mAffixData);
	}

	StrNCpy(AffixDataGet(ConfBuild, ConfBuild->nAffixData),
			AffixSet, AffixSetLen + 1);
	/* Save offset of the end of data */
	ConfBuild->AffixDataEnd += AffixSetLen + 1;
	ConfBuild->nAffixData++;
}

/*
 * Allocate space for prefix tree node.
 *
 * ConfBuild: building structure for the current dictionary.
 * array: NodeArray where to allocate new node.
 * length: number of allocated NodeData.
 * sizeNodeData: minimum size of each NodeData.
 * sizeNodeHeader: size of header of new node.
 *
 * Returns an offset of new node in NodeArray.
 */
static uint32
NIAllocateNode(IspellDictBuild *ConfBuild, NodeArray *array, uint32 length,
			   uint32 sizeNodeData, uint32 sizeNodeHeader)
{
	uint32		node_offset;
	uint32		size;

	size = sizeNodeHeader + length * sizeNodeData;

	if (array->NodesSize == 0)
	{
		array->NodesSize = size * 8;	/* Reserve space for next levels of the
										 * prefix tree */
		array->Nodes = (char *) tmpalloc(array->NodesSize);
		array->NodesEnd = 0;
	}
	else if (array->NodesEnd + size >= array->NodesSize)
	{
		array->NodesSize = Max(array->NodesSize * 2, array->NodesSize + size);
		array->Nodes = (char *) repalloc(array->Nodes, array->NodesSize);
	}

	node_offset = array->NodesEnd;
	array->NodesEnd += size;

	return node_offset;
}

/*
 * Gets an affix flag from the set of affix flags (sflagset).
 *
 * Several flags can be stored in a single string. Flags can be represented by:
 * - 1 character (FM_CHAR). A character may be Unicode.
 * - 2 characters (FM_LONG). A character may be Unicode.
 * - numbers from 1 to 65000 (FM_NUM).
 *
 * Depending on the flagmode an affix string can have the following format:
 * - FM_CHAR: ABCD
 *	 Here we have 4 flags: A, B, C and D
 * - FM_LONG: ABCDE*
 *	 Here we have 3 flags: AB, CD and E*
 * - FM_NUM: 200,205,50
 *	 Here we have 3 flags: 200, 205 and 50
 *
 * flagmode: flag mode of the dictionary
 * sflagset: the set of affix flags. Returns a reference to the start of a next
 *			 affix flag.
 * sflag: returns an affix flag from sflagset.
 */
static void
getNextFlagFromString(FlagMode *flagmode, char **sflagset, char *sflag)
{
	int32		s;
	char	   *next,
			   *sbuf = *sflagset;
	int			maxstep;
	bool		stop = false;
	bool		met_comma = false;

	maxstep = (flagmode == FM_LONG) ? 2 : 1;

	while (**sflagset)
	{
		switch (flagmode)
		{
			case FM_LONG:
			case FM_CHAR:
				COPYCHAR(sflag, *sflagset);
				sflag += pg_mblen(*sflagset);

				/* Go to start of the next flag */
				*sflagset += pg_mblen(*sflagset);

				/* Check if we get all characters of flag */
				maxstep--;
				stop = (maxstep == 0);
				break;
			case FM_NUM:
				s = strtol(*sflagset, &next, 10);
				if (*sflagset == next || errno == ERANGE)
					ereport(ERROR,
							(errcode(ERRCODE_CONFIG_FILE_ERROR),
							 errmsg("invalid affix flag \"%s\"", *sflagset)));
				if (s < 0 || s > FLAGNUM_MAXSIZE)
					ereport(ERROR,
							(errcode(ERRCODE_CONFIG_FILE_ERROR),
							 errmsg("affix flag \"%s\" is out of range",
									*sflagset)));
				sflag += sprintf(sflag, "%0d", s);

				/* Go to start of the next flag */
				*sflagset = next;
				while (**sflagset)
				{
					if (t_isdigit(*sflagset))
					{
						if (!met_comma)
							ereport(ERROR,
									(errcode(ERRCODE_CONFIG_FILE_ERROR),
									 errmsg("invalid affix flag \"%s\"",
											*sflagset)));
						break;
					}
					else if (t_iseq(*sflagset, ','))
					{
						if (met_comma)
							ereport(ERROR,
									(errcode(ERRCODE_CONFIG_FILE_ERROR),
									 errmsg("invalid affix flag \"%s\"",
											*sflagset)));
						met_comma = true;
					}
					else if (!t_isspace(*sflagset))
					{
						ereport(ERROR,
								(errcode(ERRCODE_CONFIG_FILE_ERROR),
								 errmsg("invalid character in affix flag \"%s\"",
										*sflagset)));
					}

					*sflagset += pg_mblen(*sflagset);
				}
				stop = true;
				break;
			default:
				elog(ERROR, "unrecognized type of flagmode: %d",
					 flagmode);
		}

		if (stop)
			break;
	}

	if (flagmode == FM_LONG && maxstep > 0)
		ereport(ERROR,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("invalid affix flag \"%s\" with \"long\" flag value",
						sbuf)));

	*sflag = '\0';
}

/*
 * Checks if the affix set from AffixData contains affixflag. Affix set does
 * not contain affixflag if this flag is not used actually by the .dict file.
 *
 * flagmode: flag mode of the dictionary.
 * sflagset: the set of affix flags.
 * affixflag: the affix flag.
 *
 * Returns true if the affix set string contains affixflag, otherwise returns
 * false.
 */
static bool
IsAffixFlagInUse(FlagMode *flagmode, char *sflagset, const char *affixflag)
{
	char	   *flagcur = sflagset;
	char		flag[BUFSIZ];

	if (*affixflag == 0)
		return true;

	while (*flagcur)
	{
		getNextFlagFromString(Conf->flagMode, &flagcur, flag);
		/* Compare first affix flag in flagcur with affixflag */
		if (strcmp(flag, affixflag) == 0)
			return true;
	}

	/* Could not find affixflag */
	return false;
}

/*
 * Adds the new word into the temporary array Spell.
 *
 * ConfBuild: building structure for the current dictionary.
 * word: new word.
 * flag: set of affix flags. Single flag can be get by getNextFlagFromString().
 */
static void
NIAddSpell(IspellDictBuild *ConfBuild, const char *word, const char *flag)
{
	if (ConfBuild->nSpell >= ConfBuild->mSpell)
	{
		if (ConfBuild->mSpell)
		{
			ConfBuild->mSpell *= 2;
			ConfBuild->Spell = (SPELL **) repalloc(ConfBuild->Spell,
												   ConfBuild->mSpell * sizeof(SPELL *));
		}
		else
		{
			ConfBuild->mSpell = 1024 * 20;
			ConfBuild->Spell = (SPELL **) tmpalloc(ConfBuild->mSpell * sizeof(SPELL *));
		}
	}
	ConfBuild->Spell[ConfBuild->nSpell] =
		(SPELL *) tmpalloc(SPELLHDRSZ + strlen(word) + 1);
	strcpy(ConfBuild->Spell[ConfBuild->nSpell]->word, word);
	ConfBuild->Spell[ConfBuild->nSpell]->p.flag = (*flag != '\0')
		? MemoryContextStrdup(ConfBuild->buildCxt, flag) : VoidString;

	ConfBuild->nSpell++;
}

/*
 * Imports dictionary into the temporary array Spell.
 *
 * Note caller must already have applied get_tsearch_config_filename.
 *
 * ConfBuild: building structure for the current dictionary.
 * filename: path to the .dict file.
 */
void
NIImportDictionary(IspellDictBuild *ConfBuild, const char *filename)
{
	tsearch_readline_state trst;
	char	   *line;

	if (!tsearch_readline_begin(&trst, filename))
		ereport(ERROR,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("could not open dictionary file \"%s\": %m",
						filename)));

	while ((line = tsearch_readline(&trst)) != NULL)
	{
		char	   *s,
				   *pstr;

		/* Set of affix flags */
		const char *flag;

		/* Extract flag from the line */
		flag = NULL;
		if ((s = findchar(line, '/')))
		{
			*s++ = '\0';
			flag = s;
			while (*s)
			{
				/* we allow only single encoded flags for faster works */
				if (pg_mblen(s) == 1 && t_isprint(s) && !t_isspace(s))
					s++;
				else
				{
					*s = '\0';
					break;
				}
			}
		}
		else
			flag = "";

		/* Remove trailing spaces */
		s = line;
		while (*s)
		{
			if (t_isspace(s))
			{
				*s = '\0';
				break;
			}
			s += pg_mblen(s);
		}
		pstr = lowerstr_ctx(ConfBuild, line);

		NIAddSpell(ConfBuild, pstr, flag);
		pfree(pstr);

		pfree(line);
	}
	tsearch_readline_end(&trst);
}

/*
 * Searches a basic form of word in the prefix tree. This word was generated
 * using an affix rule. This rule may not be presented in an affix set of
 * a basic form of word.
 *
 * For example, we have the entry in the .dict file:
 * meter/GMD
 *
 * The affix rule with the flag S:
 * SFX S   y	 ies		[^aeiou]y
 * is not presented here.
 *
 * The affix rule with the flag M:
 * SFX M   0	 's         .
 * is presented here.
 *
 * Conf: current dictionary.
 * word: basic form of word.
 * affixflag: affix flag, by which a basic form of word was generated.
 * flag: compound flag used to compare with StopMiddle->compoundflag.
 *
 * Returns 1 if the word was found in the prefix tree, else returns 0.
 */
static int
FindWord(IspellDict *Conf, const char *word, const char *affixflag, int flag)
{
	SPNode	   *node = Conf->Dictionary;
	SPNodeData *StopLow,
			   *StopHigh,
			   *StopMiddle;
	const uint8 *ptr = (const uint8 *) word;

	flag &= FF_COMPOUNDFLAGMASK;

	while (node && *ptr)
	{
		StopLow = node->data;
		StopHigh = node->data + node->length;
		while (StopLow < StopHigh)
		{
			StopMiddle = StopLow + ((StopHigh - StopLow) >> 1);
			if (StopMiddle->val == *ptr)
			{
				if (*(ptr + 1) == '\0' && StopMiddle->isword)
				{
					if (flag == 0)
					{
						/*
						 * The word can be formed only with another word. And
						 * in the flag parameter there is not a sign that we
						 * search compound words.
						 */
						if (StopMiddle->compoundflag & FF_COMPOUNDONLY)
							return 0;
					}
					else if ((flag & StopMiddle->compoundflag) == 0)
						return 0;

					/*
					 * Check if this affix rule is presented in the affix set
					 * with index StopMiddle->affix.
					 */
					if (IsAffixFlagInUse(Conf, StopMiddle->affix, affixflag))
						return 1;
				}
				node = StopMiddle->node;
				ptr++;
				break;
			}
			else if (StopMiddle->val < *ptr)
				StopLow = StopMiddle + 1;
			else
				StopHigh = StopMiddle;
		}
		if (StopLow >= StopHigh)
			break;
	}
	return 0;
}

/*
 * Adds a new affix rule to the Affix field.
 *
 * ConfBuild: building structure for the current dictionary, is used to allocate
 *			  temporary data.
 * flag: affix flag ('\' in the below example).
 * flagflags: set of flags from the flagval field for this affix rule. This set
 *			  is listed after '/' character in the added string (repl).
 *
 *			  For example L flag in the hunspell_sample.affix:
 *			  SFX \   0 Y/L [^Y]
 *
 * mask: condition for search ('[^Y]' in the above example).
 * find: stripping characters from beginning (at prefix) or end (at suffix)
 *		 of the word ('0' in the above example, 0 means that there is not
 *		 stripping character).
 * repl: adding string after stripping ('Y' in the above example).
 * type: FF_SUFFIX or FF_PREFIX.
 */
static void
NIAddAffix(IspellDictBuild *ConfBuild, const char *flag, char flagflags,
		   const char *mask, const char *find, const char *repl, int type)
{
	AFFIX	   *Affix;
	uint32		size;
	uint32		flaglen = strlen(flag),
				findlen = strlen(find),
				repllen = strlen(repl);

	/* Sanity checks */
	if (flaglen > AF_FLAG_MAXSIZE)
		ereport(ERROR,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("affix flag \"%s\" too long", flag)));
	if (findlen > AF_FIND_MAXSIZE)
		ereport(ERROR,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("affix find field \"%s\" too long", find)));
	if (repllen > AF_REPL_MAXSIZE)
		ereport(ERROR,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("affix repl field \"%s\" too long", repl)));

	if (ConfBuild->nAffix >= ConfBuild->mAffix)
	{
		if (ConfBuild->mAffix)
		{
			ConfBuild->mAffix *= 2;
			ConfBuild->Affix = (AFFIX **) repalloc(ConfBuild->Affix,
												   ConfBuild->mAffix * sizeof(AFFIX *));
		}
		else
		{
			ConfBuild->mAffix = 255;
			ConfBuild->Affix = (AFFIX **) tmpalloc(ConfBuild->mAffix * sizeof(AFFIX *));
		}
	}

	size = AFFIXHDRSZ + flaglen + 1 /* \0 */ + findlen + 1 /* \0 */ +
		repllen + 1 /* \0 */;

	Affix = (AFFIX *) tmpalloc(affixsize);
	ConfBuild->Affix[ConfBuild->nAffix] = Affix;

	/* This affix rule can be applied for words with any ending */
	if (strcmp(mask, ".") == 0 || *mask == '\0')
	{
		Affix->issimple = 1;
		Affix->isregis = 0;
	}
	/* This affix rule will use regis to search word ending */
	else if (RS_isRegis(mask))
	{
		Affix->issimple = 0;
		Affix->isregis = 1;
//		RS_compile(&(Affix->reg.regis), (type == FF_SUFFIX),
//				   *mask ? mask : VoidString);
	}
	/* This affix rule will use regex_t to search word ending */
	else
	{
//		int			masklen;
//		int			wmasklen;
//		int			err;
//		pg_wchar   *wmask;
//		char	   *tmask;

		Affix->issimple = 0;
		Affix->isregis = 0;
//		tmask = (char *) tmpalloc(strlen(mask) + 3);
//		if (type == FF_SUFFIX)
//			sprintf(tmask, "%s$", mask);
//		else
//			sprintf(tmask, "^%s", mask);

//		masklen = strlen(tmask);
//		wmask = (pg_wchar *) tmpalloc((masklen + 1) * sizeof(pg_wchar));
//		wmasklen = pg_mb2wchar_with_len(tmask, wmask, masklen);

//		err = pg_regcomp(&(Affix->reg.regex), wmask, wmasklen,
//						 REG_ADVANCED | REG_NOSUB,
//						 DEFAULT_COLLATION_OID);
//		if (err)
//		{
//			char		errstr[100];

//			pg_regerror(err, &(Affix->reg.regex), errstr, sizeof(errstr));
//			ereport(ERROR,
//					(errcode(ERRCODE_INVALID_REGULAR_EXPRESSION),
//					 errmsg("invalid regular expression: %s", errstr)));
//		}
	}

	Affix->flagflags = flagflags;
	if ((Affix->flagflags & FF_COMPOUNDONLY) || (Affix->flagflags & FF_COMPOUNDPERMITFLAG))
	{
		if ((Affix->flagflags & FF_COMPOUNDFLAG) == 0)
			Affix->flagflags |= FF_COMPOUNDFLAG;
	}

	Affix->type = type;

	Affix->replen = repllen;
	StrNCpy(AffixFieldRepl(Affix), repl, repllen + 1);

	Affix->findlen = findlen;
	StrNCpy(AffixFieldFind(Affix), find, findlen + 1);

	StrNCpy(AffixFieldFlag(Affix), flag, flaglen + 1);

	ConfBuild->nAffix++;
//	ConfBuild->AffixSize += affixsize;
}

/* Parsing states for parse_affentry() and friends */
#define PAE_WAIT_MASK	0
#define PAE_INMASK		1
#define PAE_WAIT_FIND	2
#define PAE_INFIND		3
#define PAE_WAIT_REPL	4
#define PAE_INREPL		5
#define PAE_WAIT_TYPE	6
#define PAE_WAIT_FLAG	7

/*
 * Parse next space-separated field of an .affix file line.
 *
 * *str is the input pointer (will be advanced past field)
 * next is where to copy the field value to, with null termination
 *
 * The buffer at "next" must be of size BUFSIZ; we truncate the input to fit.
 *
 * Returns true if we found a field, false if not.
 */
static bool
get_nextfield(char **str, char *next)
{
	int			state = PAE_WAIT_MASK;
	int			avail = BUFSIZ;

	while (**str)
	{
		if (state == PAE_WAIT_MASK)
		{
			if (t_iseq(*str, '#'))
				return false;
			else if (!t_isspace(*str))
			{
				int			clen = pg_mblen(*str);

				if (clen < avail)
				{
					COPYCHAR(next, *str);
					next += clen;
					avail -= clen;
				}
				state = PAE_INMASK;
			}
		}
		else					/* state == PAE_INMASK */
		{
			if (t_isspace(*str))
			{
				*next = '\0';
				return true;
			}
			else
			{
				int			clen = pg_mblen(*str);

				if (clen < avail)
				{
					COPYCHAR(next, *str);
					next += clen;
					avail -= clen;
				}
			}
		}
		*str += pg_mblen(*str);
	}

	*next = '\0';

	return (state == PAE_INMASK);	/* OK if we got a nonempty field */
}

/*
 * Parses entry of an .affix file of MySpell or Hunspell format.
 *
 * An .affix file entry has the following format:
 * - header
 *	 <type>  <flag>  <cross_flag>  <flag_count>
 * - fields after header:
 *	 <type>  <flag>  <find>  <replace>	<mask>
 *
 * str is the input line
 * field values are returned to type etc, which must be buffers of size BUFSIZ.
 *
 * Returns number of fields found; any omitted fields are set to empty strings.
 */
static int
parse_ooaffentry(char *str, char *type, char *flag, char *find,
				 char *repl, char *mask)
{
	int			state = PAE_WAIT_TYPE;
	int			fields_read = 0;
	bool		valid = false;

	*type = *flag = *find = *repl = *mask = '\0';

	while (*str)
	{
		switch (state)
		{
			case PAE_WAIT_TYPE:
				valid = get_nextfield(&str, type);
				state = PAE_WAIT_FLAG;
				break;
			case PAE_WAIT_FLAG:
				valid = get_nextfield(&str, flag);
				state = PAE_WAIT_FIND;
				break;
			case PAE_WAIT_FIND:
				valid = get_nextfield(&str, find);
				state = PAE_WAIT_REPL;
				break;
			case PAE_WAIT_REPL:
				valid = get_nextfield(&str, repl);
				state = PAE_WAIT_MASK;
				break;
			case PAE_WAIT_MASK:
				valid = get_nextfield(&str, mask);
				state = -1;		/* force loop exit */
				break;
			default:
				elog(ERROR, "unrecognized state in parse_ooaffentry: %d",
					 state);
				break;
		}
		if (valid)
			fields_read++;
		else
			break;				/* early EOL */
		if (state < 0)
			break;				/* got all fields */
	}

	return fields_read;
}

/*
 * Parses entry of an .affix file of Ispell format
 *
 * An .affix file entry has the following format:
 * <mask>  >  [-<find>,]<replace>
 */
static bool
parse_affentry(char *str, char *mask, char *find, char *repl)
{
	int			state = PAE_WAIT_MASK;
	char	   *pmask = mask,
			   *pfind = find,
			   *prepl = repl;

	*mask = *find = *repl = '\0';

	while (*str)
	{
		if (state == PAE_WAIT_MASK)
		{
			if (t_iseq(str, '#'))
				return false;
			else if (!t_isspace(str))
			{
				COPYCHAR(pmask, str);
				pmask += pg_mblen(str);
				state = PAE_INMASK;
			}
		}
		else if (state == PAE_INMASK)
		{
			if (t_iseq(str, '>'))
			{
				*pmask = '\0';
				state = PAE_WAIT_FIND;
			}
			else if (!t_isspace(str))
			{
				COPYCHAR(pmask, str);
				pmask += pg_mblen(str);
			}
		}
		else if (state == PAE_WAIT_FIND)
		{
			if (t_iseq(str, '-'))
			{
				state = PAE_INFIND;
			}
			else if (t_isalpha(str) || t_iseq(str, '\'') /* english 's */ )
			{
				COPYCHAR(prepl, str);
				prepl += pg_mblen(str);
				state = PAE_INREPL;
			}
			else if (!t_isspace(str))
				ereport(ERROR,
						(errcode(ERRCODE_CONFIG_FILE_ERROR),
						 errmsg("syntax error")));
		}
		else if (state == PAE_INFIND)
		{
			if (t_iseq(str, ','))
			{
				*pfind = '\0';
				state = PAE_WAIT_REPL;
			}
			else if (t_isalpha(str))
			{
				COPYCHAR(pfind, str);
				pfind += pg_mblen(str);
			}
			else if (!t_isspace(str))
				ereport(ERROR,
						(errcode(ERRCODE_CONFIG_FILE_ERROR),
						 errmsg("syntax error")));
		}
		else if (state == PAE_WAIT_REPL)
		{
			if (t_iseq(str, '-'))
			{
				break;			/* void repl */
			}
			else if (t_isalpha(str))
			{
				COPYCHAR(prepl, str);
				prepl += pg_mblen(str);
				state = PAE_INREPL;
			}
			else if (!t_isspace(str))
				ereport(ERROR,
						(errcode(ERRCODE_CONFIG_FILE_ERROR),
						 errmsg("syntax error")));
		}
		else if (state == PAE_INREPL)
		{
			if (t_iseq(str, '#'))
			{
				*prepl = '\0';
				break;
			}
			else if (t_isalpha(str))
			{
				COPYCHAR(prepl, str);
				prepl += pg_mblen(str);
			}
			else if (!t_isspace(str))
				ereport(ERROR,
						(errcode(ERRCODE_CONFIG_FILE_ERROR),
						 errmsg("syntax error")));
		}
		else
			elog(ERROR, "unrecognized state in parse_affentry: %d", state);

		str += pg_mblen(str);
	}

	*pmask = *pfind = *prepl = '\0';

	return (*mask && (*find || *repl));
}

/*
 * Sets a Hunspell options depending on flag type.
 */
static void
setCompoundAffixFlagValue(IspellDictBuild *ConfBuild, CompoundAffixFlag *entry,
						  char *s, uint32 val)
{
	if (ConfBuild->dict->flagMode == FM_NUM)
	{
		char	   *next;
		int			i;

		i = strtol(s, &next, 10);
		if (s == next || errno == ERANGE)
			ereport(ERROR,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("invalid affix flag \"%s\"", s)));
		if (i < 0 || i > FLAGNUM_MAXSIZE)
			ereport(ERROR,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("affix flag \"%s\" is out of range", s)));

		entry->flag.i = i;
	}
	else
		entry->flag.s = MemoryContextStrdup(ConfBuild->buildCxt, s);

	entry->flagMode = ConfBuild->dict->flagMode;
	entry->value = val;
}

/*
 * Sets up a correspondence for the affix parameter with the affix flag.
 *
 * ConfBuild: building structure for the current dictionary.
 * s: affix flag in string.
 * val: affix parameter.
 */
static void
addCompoundAffixFlagValue(IspellDictBuild *ConfBuild, char *s, uint32 val)
{
	CompoundAffixFlag *newValue;
	char		sbuf[BUFSIZ];
	char	   *sflag;
	int			clen;

	while (*s && t_isspace(s))
		s += pg_mblen(s);

	if (!*s)
		ereport(ERROR,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("syntax error")));

	/* Get flag without \n */
	sflag = sbuf;
	while (*s && !t_isspace(s) && *s != '\n')
	{
		clen = pg_mblen(s);
		COPYCHAR(sflag, s);
		sflag += clen;
		s += clen;
	}
	*sflag = '\0';

	/* Resize array or allocate memory for array CompoundAffixFlag */
	if (ConfBuild->nCompoundAffixFlag >= ConfBuild->mCompoundAffixFlag)
	{
		if (ConfBuild->mCompoundAffixFlag)
		{
			ConfBuild->mCompoundAffixFlag *= 2;
			ConfBuild->CompoundAffixFlags = (CompoundAffixFlag *)
				repalloc((void *) ConfBuild->CompoundAffixFlags,
						 ConfBuild->mCompoundAffixFlag * sizeof(CompoundAffixFlag));
		}
		else
		{
			ConfBuild->mCompoundAffixFlag = 10;
			ConfBuild->CompoundAffixFlags = (CompoundAffixFlag *)
				tmpalloc(ConfBuild->mCompoundAffixFlag * sizeof(CompoundAffixFlag));
		}
	}

	newValue = ConfBuild->CompoundAffixFlags + ConfBuild->nCompoundAffixFlag;

	setCompoundAffixFlagValue(ConfBuild, newValue, sbuf, val);

	ConfBuild->dict->usecompound = true;
	ConfBuild->nCompoundAffixFlag++;
}

/*
 * Returns a set of affix parameters which correspondence to the set of affix
 * flags s.
 */
static int
getCompoundAffixFlagValue(IspellDictBuild *ConfBuild, char *s)
{
	uint32		flag = 0;
	CompoundAffixFlag *found,
				key;
	char		sflag[BUFSIZ];
	char	   *flagcur;

	if (ConfBuild->nCompoundAffixFlag == 0)
		return 0;

	flagcur = s;
	while (*flagcur)
	{
		getNextFlagFromString(ConfBuild->dict->flagMode, &flagcur, sflag);
		setCompoundAffixFlagValue(ConfBuild, &key, sflag, 0);

		found = (CompoundAffixFlag *)
			bsearch(&key, (void *) ConfBuild->CompoundAffixFlags,
					ConfBuild->nCompoundAffixFlag, sizeof(CompoundAffixFlag),
					cmpcmdflag);
		if (found != NULL)
			flag |= found->value;
	}

	return flag;
}

/*
 * Returns a flag set using the s parameter.
 *
 * If useFlagAliases is true then the s parameter is index of the AffixData
 * array and function returns its entry.  Else function returns the s parameter.
 */
static char *
getAffixFlagSet(IspellDictBuild *ConfBuild, char *s)
{
	if (ConfBuild->dict->useFlagAliases && *s != '\0')
	{
		int			curaffix;
		char	   *end;

		curaffix = strtol(s, &end, 10);
		if (s == end || errno == ERANGE)
			ereport(ERROR,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("invalid affix alias \"%s\"", s)));

		if (curaffix > 0 && curaffix <= ConfBuild->nAffixData)

			/*
			 * Do not subtract 1 from curaffix because empty string was added
			 * in NIImportOOAffixes
			 */
			return AffixDataGet(ConfBuild, curaffix);
		else
			return VoidString;
	}
	else
		return s;
}

/*
 * Import an affix file that follows MySpell or Hunspell format.
 *
 * ConfBuild: building structure for the current dictionary.
 * filename: path to the .affix file.
 */
static void
NIImportOOAffixes(IspellDictBuild *ConfBuild, const char *filename)
{
	char		type[BUFSIZ],
			   *ptype = NULL;
	char		sflag[BUFSIZ];
	char		mask[BUFSIZ],
			   *pmask;
	char		find[BUFSIZ],
			   *pfind;
	char		repl[BUFSIZ],
			   *prepl;
	bool		isSuffix = false;
	int			naffix = 0;
	int			sflaglen = 0;
	char		flagflags = 0;
	tsearch_readline_state trst;
	char	   *recoded;

	/* read file to find any flag */
	ConfBuild->dict->usecompound = false;
	ConfBuild->dict->useFlagAliases = false;
	ConfBuild->dict->flagMode = FM_CHAR;

	if (!tsearch_readline_begin(&trst, filename))
		ereport(ERROR,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("could not open affix file \"%s\": %m",
						filename)));

	while ((recoded = tsearch_readline(&trst)) != NULL)
	{
		if (*recoded == '\0' || t_isspace(recoded) || t_iseq(recoded, '#'))
		{
			pfree(recoded);
			continue;
		}

		if (STRNCMP(recoded, "COMPOUNDFLAG") == 0)
			addCompoundAffixFlagValue(ConfBuild,
									  recoded + strlen("COMPOUNDFLAG"),
									  FF_COMPOUNDFLAG);
		else if (STRNCMP(recoded, "COMPOUNDBEGIN") == 0)
			addCompoundAffixFlagValue(ConfBuild,
									  recoded + strlen("COMPOUNDBEGIN"),
									  FF_COMPOUNDBEGIN);
		else if (STRNCMP(recoded, "COMPOUNDLAST") == 0)
			addCompoundAffixFlagValue(ConfBuild,
									  recoded + strlen("COMPOUNDLAST"),
									  FF_COMPOUNDLAST);
		/* COMPOUNDLAST and COMPOUNDEND are synonyms */
		else if (STRNCMP(recoded, "COMPOUNDEND") == 0)
			addCompoundAffixFlagValue(ConfBuild,
									  recoded + strlen("COMPOUNDEND"),
									  FF_COMPOUNDLAST);
		else if (STRNCMP(recoded, "COMPOUNDMIDDLE") == 0)
			addCompoundAffixFlagValue(ConfBuild,
									  recoded + strlen("COMPOUNDMIDDLE"),
									  FF_COMPOUNDMIDDLE);
		else if (STRNCMP(recoded, "ONLYINCOMPOUND") == 0)
			addCompoundAffixFlagValue(ConfBuild,
									  recoded + strlen("ONLYINCOMPOUND"),
									  FF_COMPOUNDONLY);
		else if (STRNCMP(recoded, "COMPOUNDPERMITFLAG") == 0)
			addCompoundAffixFlagValue(ConfBuild,
									  recoded + strlen("COMPOUNDPERMITFLAG"),
									  FF_COMPOUNDPERMITFLAG);
		else if (STRNCMP(recoded, "COMPOUNDFORBIDFLAG") == 0)
			addCompoundAffixFlagValue(ConfBuild,
									  recoded + strlen("COMPOUNDFORBIDFLAG"),
									  FF_COMPOUNDFORBIDFLAG);
		else if (STRNCMP(recoded, "FLAG") == 0)
		{
			char	   *s = recoded + strlen("FLAG");

			while (*s && t_isspace(s))
				s += pg_mblen(s);

			if (*s)
			{
				if (STRNCMP(s, "long") == 0)
					ConfBuild->dict->flagMode = FM_LONG;
				else if (STRNCMP(s, "num") == 0)
					ConfBuild->dict->flagMode = FM_NUM;
				else if (STRNCMP(s, "default") != 0)
					ereport(ERROR,
							(errcode(ERRCODE_CONFIG_FILE_ERROR),
							 errmsg("Ispell dictionary supports only "
									"\"default\", \"long\", "
									"and \"num\" flag values")));
			}
		}

		pfree(recoded);
	}
	tsearch_readline_end(&trst);

	if (ConfBuild->nCompoundAffixFlag > 1)
		qsort((void *) ConfBuild->CompoundAffixFlags, ConfBuild->nCompoundAffixFlag,
			  sizeof(CompoundAffixFlag), cmpcmdflag);

	if (!tsearch_readline_begin(&trst, filename))
		ereport(ERROR,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("could not open affix file \"%s\": %m",
						filename)));

	while ((recoded = tsearch_readline(&trst)) != NULL)
	{
		int			fields_read;

		if (*recoded == '\0' || t_isspace(recoded) || t_iseq(recoded, '#'))
			goto nextline;

		fields_read = parse_ooaffentry(recoded, type, sflag, find, repl, mask);

		if (ptype)
			pfree(ptype);
		ptype = lowerstr_ctx(ConfBuild, type);

		/* First try to parse AF parameter (alias compression) */
		if (STRNCMP(ptype, "af") == 0)
		{
			/* First line is the number of aliases */
			if (!ConfBuild->dict->useFlagAliases)
			{
				ConfBuild->dict->useFlagAliases = true;
				naffix = atoi(sflag);
				if (naffix == 0)
					ereport(ERROR,
							(errcode(ERRCODE_CONFIG_FILE_ERROR),
							 errmsg("invalid number of flag vector aliases")));

				/* Also reserve place for empty flag set */
				naffix++;

				NIInitAffixData(ConfBuild, naffix);

				/* Add empty flag set into AffixData */
				NIAddAffixSet(ConfBuild, VoidString, 0);
			}
			/* Other lines is aliases */
			else
			{
				NIAddAffixSet(ConfBuild, sflag, strlen(sflag));
			}
			goto nextline;
		}
		/* Else try to parse prefixes and suffixes */
		if (fields_read < 4 ||
			(STRNCMP(ptype, "sfx") != 0 && STRNCMP(ptype, "pfx") != 0))
			goto nextline;

		sflaglen = strlen(sflag);
		if (sflaglen == 0
			|| (sflaglen > 1 && ConfBuild->dict->flagMode == FM_CHAR)
			|| (sflaglen > 2 && ConfBuild->dict->flagMode == FM_LONG))
			goto nextline;

		/*--------
		 * Affix header. For example:
		 * SFX \ N 1
		 *--------
		 */
		if (fields_read == 4)
		{
			isSuffix = (STRNCMP(ptype, "sfx") == 0);
			if (t_iseq(find, 'y') || t_iseq(find, 'Y'))
				flagflags = FF_CROSSPRODUCT;
			else
				flagflags = 0;
		}
		/*--------
		 * Affix fields. For example:
		 * SFX \   0	Y/L [^Y]
		 *--------
		 */
		else
		{
			char	   *ptr;
			int			aflg = 0;

			/* Get flags after '/' (flags are case sensitive) */
			if ((ptr = strchr(repl, '/')) != NULL)
				aflg |= getCompoundAffixFlagValue(ConfBuild,
												  getAffixFlagSet(ConfBuild->dict,
																  ptr + 1));
			/* Get lowercased version of string before '/' */
			prepl = lowerstr_ctx(ConfBuild, repl);
			if ((ptr = strchr(prepl, '/')) != NULL)
				*ptr = '\0';
			pfind = lowerstr_ctx(ConfBuild, find);
			pmask = lowerstr_ctx(ConfBuild, mask);
			if (t_iseq(find, '0'))
				*pfind = '\0';
			if (t_iseq(repl, '0'))
				*prepl = '\0';

			NIAddAffix(ConfBuild, sflag, flagflags | aflg, pmask, pfind, prepl,
					   isSuffix ? FF_SUFFIX : FF_PREFIX);
			pfree(prepl);
			pfree(pfind);
			pfree(pmask);
		}

nextline:
		pfree(recoded);
	}

	tsearch_readline_end(&trst);
	if (ptype)
		pfree(ptype);
}

/*
 * import affixes
 *
 * Note caller must already have applied get_tsearch_config_filename
 *
 * This function is responsible for parsing ispell ("old format") affix files.
 * If we realize that the file contains new-format commands, we pass off the
 * work to NIImportOOAffixes(), which will re-read the whole file.
 */
void
NIImportAffixes(IspellDictBuild *ConfBuild, const char *filename)
{
	char	   *pstr = NULL;
	char		flag[BUFSIZ];
	char		mask[BUFSIZ];
	char		find[BUFSIZ];
	char		repl[BUFSIZ];
	char	   *s;
	bool		suffixes = false;
	bool		prefixes = false;
	char		flagflags = 0;
	tsearch_readline_state trst;
	bool		oldformat = false;
	char	   *recoded = NULL;

	if (!tsearch_readline_begin(&trst, filename))
		ereport(ERROR,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("could not open affix file \"%s\": %m",
						filename)));

	ConfBuild->dict->usecompound = false;
	ConfBuild->dict->useFlagAliases = false;
	ConfBuild->dict->flagMode = FM_CHAR;

	while ((recoded = tsearch_readline(&trst)) != NULL)
	{
		pstr = lowerstr(recoded);

		/* Skip comments and empty lines */
		if (*pstr == '#' || *pstr == '\n')
			goto nextline;

		if (STRNCMP(pstr, "compoundwords") == 0)
		{
			/* Find case-insensitive L flag in non-lowercased string */
			s = findchar2(recoded, 'l', 'L');
			if (s)
			{
				while (*s && !t_isspace(s))
					s += pg_mblen(s);
				while (*s && t_isspace(s))
					s += pg_mblen(s);

				if (*s && pg_mblen(s) == 1)
					addCompoundAffixFlagValue(ConfBuild, s, FF_COMPOUNDFLAG);

				oldformat = true;
				goto nextline;
			}
		}
		if (STRNCMP(pstr, "suffixes") == 0)
		{
			suffixes = true;
			prefixes = false;
			oldformat = true;
			goto nextline;
		}
		if (STRNCMP(pstr, "prefixes") == 0)
		{
			suffixes = false;
			prefixes = true;
			oldformat = true;
			goto nextline;
		}
		if (STRNCMP(pstr, "flag") == 0)
		{
			s = recoded + 4;	/* we need non-lowercased string */
			flagflags = 0;

			while (*s && t_isspace(s))
				s += pg_mblen(s);

			if (*s == '*')
			{
				flagflags |= FF_CROSSPRODUCT;
				s++;
			}
			else if (*s == '~')
			{
				flagflags |= FF_COMPOUNDONLY;
				s++;
			}

			if (*s == '\\')
				s++;

			/*
			 * An old-format flag is a single ASCII character; we expect it to
			 * be followed by EOL, whitespace, or ':'.  Otherwise this is a
			 * new-format flag command.
			 */
			if (*s && pg_mblen(s) == 1)
			{
				COPYCHAR(flag, s);
				flag[1] = '\0';

				s++;
				if (*s == '\0' || *s == '#' || *s == '\n' || *s == ':' ||
					t_isspace(s))
				{
					oldformat = true;
					goto nextline;
				}
			}
			goto isnewformat;
		}
		if (STRNCMP(recoded, "COMPOUNDFLAG") == 0 ||
			STRNCMP(recoded, "COMPOUNDMIN") == 0 ||
			STRNCMP(recoded, "PFX") == 0 ||
			STRNCMP(recoded, "SFX") == 0)
			goto isnewformat;

		if ((!suffixes) && (!prefixes))
			goto nextline;

		if (!parse_affentry(pstr, mask, find, repl))
			goto nextline;

		NIAddAffix(ConfBuild, flag, flagflags, mask, find, repl,
				   suffixes ? FF_SUFFIX : FF_PREFIX);

nextline:
		pfree(recoded);
		pfree(pstr);
	}
	tsearch_readline_end(&trst);
	return;

isnewformat:
	pfree(recoded);
	pfree(pstr);

	if (oldformat)
		ereport(ERROR,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("affix file contains both old-style and new-style commands")));
	tsearch_readline_end(&trst);

	NIImportOOAffixes(ConfBuild, filename);
}

/*
 * Merges two affix flag sets and stores a new affix flag set into
 * ConfBuild->AffixData.
 *
 * Returns index of a new affix flag set.
 */
static int
MergeAffix(IspellDictBuild *ConfBuild, int a1, int a2)
{
	char	   *ptr;
	uint32		len;

	/* Do not merge affix flags if one of affix flags is empty */
	if (*AffixDataGet(ConfBuild, a1) == '\0')
		return a2;
	else if (*AffixDataGet(ConfBuild, a2) == '\0')
		return a1;

	if (Conf->flagMode == FM_NUM)
	{
		len = strlen(Conf->AffixData[a1]) + strlen(Conf->AffixData[a2]) +
			1 /* comma */;
		ptr = tmpalloc(len + 1 /* \0 */);
		sprintf(ptr, "%s,%s", Conf->AffixData[a1], Conf->AffixData[a2]);
	}
	else
	{
		len = strlen(Conf->AffixData[a1]) + strlen(Conf->AffixData[a2]);
		ptr = tmpalloc(len + 1 /* \0 */ );
		sprintf(ptr, "%s%s", Conf->AffixData[a1], Conf->AffixData[a2]);
	}

	NIAddAffixSet(ConfBuild, ptr, len);
	pfree(ptr);

	return ConfBuild->AffixData->length - 1;
}

/*
 * Returns a set of affix parameters which correspondence to the set of affix
 * flags with the given index.
 */
static uint32
makeCompoundFlags(IspellDict *Conf, IspellDictBuild *ConfBuild, int affix)
{
	char	   *str = Conf->AffixData[affix];

	return (getCompoundAffixFlagValue(ConfBuild, str) & FF_COMPOUNDFLAGMASK);
}

/*
 * Makes a prefix tree for the given level.
 *
 * ConfBuild: building structure for the current dictionary.
 * low: lower index of the Conf->Spell array.
 * high: upper index of the Conf->Spell array.
 * level: current prefix tree level.
 *
 * Returns an offset of SPNode in DictNodes.
 */
static uint32
mkSPNode(IspellDictBuild *ConfBuild, int low, int high, int level)
{
	int			i;
	int			nchar = 0;
	char		lastchar = '\0';
	uint32		rs_offset;
	SPNode	   *rs;
	SPNodeData *data;
	int			lownew = low;

	for (i = low; i < high; i++)
		if (ConfBuild->Spell[i]->p.d.len > level &&
			lastchar != ConfBuild->Spell[i]->word[level])
		{
			nchar++;
			lastchar = ConfBuild->Spell[i]->word[level];
		}

	if (!nchar)
		return ISPELL_INVALID_OFFSET;

	rs_offset = NIAllocateNode(ConfBuild, &ConfBuild->DictNodes, nchar,
							   sizeof(SPNodeData), SPNHDRSZ);
	rs = NodeArrayGet(&ConfBuild->DictNodes, rs_offset);
	rs->length = nchar;
	data = rs->data;

	lastchar = '\0';
	for (i = low; i < high; i++)
		if (ConfBuild->Spell[i]->p.d.len > level)
		{
			if (lastchar != ConfBuild->Spell[i]->word[level])
			{
				if (lastchar)
				{
					/* Next level of the prefix tree */
					data->node_offset = mkSPNode(ConfBuild, lownew, i, level + 1);
					lownew = i;
					data++;
				}
				lastchar = ConfBuild->Spell[i]->word[level];
			}
			data->val = ((uint8 *) (ConfBuild->Spell[i]->word))[level];
			if (ConfBuild->Spell[i]->p.d.len == level + 1)
			{
				bool		clearCompoundOnly = false;

				if (data->isword && data->affix != ConfBuild->Spell[i]->p.d.affix)
				{
					/*
					 * MergeAffix called a few times. If one of word is
					 * allowed to be in compound word and another isn't, then
					 * clear FF_COMPOUNDONLY flag.
					 */

					clearCompoundOnly = (FF_COMPOUNDONLY & data->compoundflag
										 & makeCompoundFlags(Conf, ConfBuild,
															 ConfBuild->Spell[i]->p.d.affix))
						? false : true;
					data->affix = MergeAffix(ConfBuild, data->affix,
											 ConfBuild->Spell[i]->p.d.affix);
				}
				else
					data->affix = ConfBuild->Spell[i]->p.d.affix;
				data->isword = 1;

				data->compoundflag = makeCompoundFlags(Conf, ConfBuild,
													   data->affix);

				if ((data->compoundflag & FF_COMPOUNDONLY) &&
					(data->compoundflag & FF_COMPOUNDFLAG) == 0)
					data->compoundflag |= FF_COMPOUNDFLAG;

				if (clearCompoundOnly)
					data->compoundflag &= ~FF_COMPOUNDONLY;
			}
		}

	/* Next level of the prefix tree */
	data->node_offset = mkSPNode(ConfBuild, lownew, high, level + 1);

	return rs_offset;
}

/*
 * Builds the Conf->Dictionary tree and AffixData from the imported dictionary
 * and affixes.
 */
void
NISortDictionary(IspellDictBuild *ConfBuild)
{
	int			i;
	int			naffix = 0;
	int			curaffix;

	/* compress affixes */

	/*
	 * If we use flag aliases then we need to use Conf->AffixData filled in
	 * the NIImportOOAffixes().
	 */
	if (ConfBuild->dict->useFlagAliases)
	{
		for (i = 0; i < ConfBuild->nSpell; i++)
		{
			char	   *end;

			if (*ConfBuild->Spell[i]->p.flag != '\0')
			{
				curaffix = strtol(ConfBuild->Spell[i]->p.flag, &end, 10);
				if (ConfBuild->Spell[i]->p.flag == end || errno == ERANGE)
					ereport(ERROR,
							(errcode(ERRCODE_CONFIG_FILE_ERROR),
							 errmsg("invalid affix alias \"%s\"",
									ConfBuild->Spell[i]->p.flag)));
			}
			else
			{
				/*
				 * If ConfBuild->Spell[i]->p.flag is empty, then get empty value of
				 * Conf->AffixData (0 index).
				 */
				curaffix = 0;
			}

			ConfBuild->Spell[i]->p.d.affix = curaffix;
			ConfBuild->Spell[i]->p.d.len = strlen(ConfBuild->Spell[i]->word);
		}
	}
	/* Otherwise fill Conf->AffixData here */
	else
	{
		/* Count the number of different flags used in the dictionary */
		qsort((void *) ConfBuild->Spell, ConfBuild->nSpell, sizeof(SPELL *),
			  cmpspellaffix);

		naffix = 0;
		for (i = 0; i < ConfBuild->nSpell; i++)
		{
			if (i == 0
				|| strcmp(ConfBuild->Spell[i]->p.flag,
						  ConfBuild->Spell[i - 1]->p.flag))
				naffix++;
		}

		/*
		 * Fill in AffixData with the affixes that were used in the
		 * dictionary. Replace textual flag-field of ConfBuild->Spell entries
		 * with indexes into Conf->AffixData array.
		 */
		NIInitAffixData(ConfBuild, naffix);

		curaffix = -1;
		for (i = 0; i < ConfBuild->nSpell; i++)
		{
			if (i == 0
				|| strcmp(ConfBuild->Spell[i]->p.flag,
						  AffixDataGet(ConfBuild, curaffix)))
			{
				curaffix++;
				Assert(curaffix < naffix);
				NIAddAffixSet(ConfBuild, ConfBuild->Spell[i]->p.flag,
								   strlen(ConfBuild->Spell[i]->p.flag));
			}

			ConfBuild->Spell[i]->p.d.affix = curaffix;
			ConfBuild->Spell[i]->p.d.len = strlen(ConfBuild->Spell[i]->word);
		}
	}

	/* Start build a prefix tree */
	qsort((void *) ConfBuild->Spell, ConfBuild->nSpell, sizeof(SPELL *), cmpspell);
	mkSPNode(ConfBuild, 0, ConfBuild->nSpell, 0);
}

/*
 * Makes a prefix tree for the given level using the repl string of an affix
 * rule. Affixes with empty replace string do not include in the prefix tree.
 * This affixes are included by mkVoidAffix().
 *
 * ConfBuild: building structure for the current dictionary.
 * low: lower index of the Conf->Affix array.
 * high: upper index of the Conf->Affix array.
 * level: current prefix tree level.
 * type: FF_SUFFIX or FF_PREFIX.
 */
static AffixNode *
mkANode(IspellDict *Conf, IspellDictBuild *ConfBuild,
		int low, int high, int level, int type)
{
	int			i;
	int			nchar = 0;
	uint8		lastchar = '\0';
	AffixNode  *rs;
	AffixNodeData *data;
	int			lownew = low;
	int			naff;
	AFFIX	  **aff;

	for (i = low; i < high; i++)
		if (Conf->Affix[i].replen > level && lastchar != GETCHAR(Conf->Affix + i, level, type))
		{
			nchar++;
			lastchar = GETCHAR(Conf->Affix + i, level, type);
		}

	if (!nchar)
		return NULL;

	aff = (AFFIX **) tmpalloc(sizeof(AFFIX *) * (high - low + 1));
	naff = 0;

	rs = (AffixNode *) cpalloc0(ANHRDSZ + nchar * sizeof(AffixNodeData));
	rs->length = nchar;
	data = rs->data;

	lastchar = '\0';
	for (i = low; i < high; i++)
		if (Conf->Affix[i].replen > level)
		{
			if (lastchar != GETCHAR(Conf->Affix + i, level, type))
			{
				if (lastchar)
				{
					/* Next level of the prefix tree */
					data->node = mkANode(Conf, ConfBuild, lownew, i, level + 1,
										 type);
					if (naff)
					{
						data->naff = naff;
						data->aff = (AFFIX **) cpalloc(sizeof(AFFIX *) * naff);
						memcpy(data->aff, aff, sizeof(AFFIX *) * naff);
						naff = 0;
					}
					data++;
					lownew = i;
				}
				lastchar = GETCHAR(Conf->Affix + i, level, type);
			}
			data->val = GETCHAR(Conf->Affix + i, level, type);
			if (Conf->Affix[i].replen == level + 1)
			{					/* affix stopped */
				aff[naff++] = Conf->Affix + i;
			}
		}

	/* Next level of the prefix tree */
	data->node = mkANode(Conf, ConfBuild, lownew, high, level + 1, type);
	if (naff)
	{
		data->naff = naff;
		data->aff = (AFFIX **) cpalloc(sizeof(AFFIX *) * naff);
		memcpy(data->aff, aff, sizeof(AFFIX *) * naff);
		naff = 0;
	}

	pfree(aff);

	return rs;
}

/*
 * Makes the root void node in the prefix tree. The root void node is created
 * for affixes which have empty replace string ("repl" field).
 */
static void
mkVoidAffix(IspellDictBuild *ConfBuild, bool issuffix, int startsuffix)
{
	int			i,
				cnt = 0;
	int			start = (issuffix) ? startsuffix : 0;
	int			end = (issuffix) ? ConfBuild->nAffix : startsuffix;
	AffixNode  *Affix = (AffixNode *) palloc0(ANHRDSZ + sizeof(AffixNodeData));

	/* Count affixes with empty replace string */
	for (i = start; i < end; i++)
		if (ConfBuild->Affix[i]->replen == 0)
			cnt++;

	Affix->length = 1;
	Affix->isvoid = 1;

	if (issuffix)
	{
		Affix->data->node = ConfBuild->Suffix;
		ConfBuild->Suffix = Affix;
	}
	else
	{
		Affix->data->node = ConfBuild->Prefix;
		ConfBuild->Prefix = Affix;
	}

	/* There is not affixes with empty replace string */
	if (cnt == 0)
		return;

	Affix->data->aff = (AFFIX **) cpalloc(sizeof(AFFIX *) * cnt);
	Affix->data->naff = (uint32) cnt;

	cnt = 0;
	for (i = start; i < end; i++)
		if (ConfBuild->Affix[i]->replen == 0)
		{
			Affix->data->aff[cnt] = ConfBuild->Affix + i;
			cnt++;
		}
}

/*
 * Checks if the affixflag is used by dictionary. AffixData does not
 * contain affixflag if this flag is not used actually by the .dict file.
 *
 * ConfBuild: building structure for the current dictionary.
 * affixflag: affix flag.
 *
 * Returns true if the Conf->AffixData array contains affixflag, otherwise
 * returns false.
 */
static bool
isAffixInUse(IspellDictBuild *ConfBuild, char *affixflag)
{
	int			i;

	for (i = 0; i < ConfBuild->nAffixData; i++)
		if (IsAffixFlagInUse(ConfBuild->dict->flagMode,
							 AffixDataGet(ConfBuild, i), affixflag))
			return true;

	return false;
}

/*
 * Builds Prefix and Suffix trees from the imported affixes.
 */
void
NISortAffixes(IspellDictBuild *ConfBuild)
{
	AFFIX	   *Affix;
	size_t		i;
	CMPDAffix  *ptr;
	int			firstsuffix = ConfBuild->nAffix;

	if (ConfBuild->nAffix == 0)
		return;

	/* Store compound affixes in the Conf->CompoundAffix array */
	if (ConfBuild->nAffix > 1)
		qsort((void *) ConfBuild->Affix, ConfBuild->nAffix,
			  sizeof(AFFIX *), cmpaffix);
	ConfBuild->CompoundAffix = ptr =
		(CMPDAffix *) tmpalloc(sizeof(CMPDAffix) * ConfBuild->nAffix);
	ptr->affix = ISPELL_INVALID_INDEX;

	for (i = 0; i < ConfBuild->nAffix; i++)
	{
		Affix = ConfBuild->Affix[i];
		if (Affix->type == FF_SUFFIX && i < firstsuffix)
			firstsuffix = i;

		if ((Affix->flagflags & FF_COMPOUNDFLAG) && Affix->replen > 0 &&
			isAffixInUse(ConfBuild, AffixFieldFlag(Affix)))
		{
			if (ptr == Conf->CompoundAffix ||
				ptr->issuffix != (ptr - 1)->issuffix ||
				strbncmp((const unsigned char *) (ptr - 1)->affix,
						 (const unsigned char *) AffixFieldRepl(Affix),
						 (ptr - 1)->len))
			{
				/* leave only unique and minimals suffixes */
				ptr->affix = i;
				ptr->len = Affix->replen;
				ptr->issuffix = (Affix->type == FF_SUFFIX);
				ptr++;
			}
		}
	}
	ptr->affix = ISPELL_INVALID_INDEX;
	ConfBuild->CompoundAffix = (CMPDAffix *) repalloc(Conf->CompoundAffix,
												 sizeof(CMPDAffix) * (ptr - Conf->CompoundAffix + 1));

	/* Start build a prefix tree */
	Conf->Prefix = mkANode(Conf, ConfBuild, 0, firstsuffix, 0, FF_PREFIX);
	Conf->Suffix = mkANode(Conf, ConfBuild, firstsuffix, Conf->naffixes, 0, FF_SUFFIX);
	mkVoidAffix(Conf, true, firstsuffix);
	mkVoidAffix(Conf, false, firstsuffix);
}

static AffixNodeData *
FindAffixes(AffixNode *node, const char *word, int wrdlen, int *level, int type)
{
	AffixNodeData *StopLow,
			   *StopHigh,
			   *StopMiddle;
	uint8 symbol;

	if (node->isvoid)
	{							/* search void affixes */
		if (node->data->naff)
			return node->data;
		node = node->data->node;
	}

	while (node && *level < wrdlen)
	{
		StopLow = node->data;
		StopHigh = node->data + node->length;
		while (StopLow < StopHigh)
		{
			StopMiddle = StopLow + ((StopHigh - StopLow) >> 1);
			symbol = GETWCHAR(word, wrdlen, *level, type);

			if (StopMiddle->val == symbol)
			{
				(*level)++;
				if (StopMiddle->naff)
					return StopMiddle;
				node = StopMiddle->node;
				break;
			}
			else if (StopMiddle->val < symbol)
				StopLow = StopMiddle + 1;
			else
				StopHigh = StopMiddle;
		}
		if (StopLow >= StopHigh)
			break;
	}
	return NULL;
}

static char *
CheckAffix(const char *word, size_t len, AFFIX *Affix, int flagflags, char *newword, int *baselen)
{
	/*
	 * Check compound allow flags
	 */

	if (flagflags == 0)
	{
		if (Affix->flagflags & FF_COMPOUNDONLY)
			return NULL;
	}
	else if (flagflags & FF_COMPOUNDBEGIN)
	{
		if (Affix->flagflags & FF_COMPOUNDFORBIDFLAG)
			return NULL;
		if ((Affix->flagflags & FF_COMPOUNDBEGIN) == 0)
			if (Affix->type == FF_SUFFIX)
				return NULL;
	}
	else if (flagflags & FF_COMPOUNDMIDDLE)
	{
		if ((Affix->flagflags & FF_COMPOUNDMIDDLE) == 0 ||
			(Affix->flagflags & FF_COMPOUNDFORBIDFLAG))
			return NULL;
	}
	else if (flagflags & FF_COMPOUNDLAST)
	{
		if (Affix->flagflags & FF_COMPOUNDFORBIDFLAG)
			return NULL;
		if ((Affix->flagflags & FF_COMPOUNDLAST) == 0)
			if (Affix->type == FF_PREFIX)
				return NULL;
	}

	/*
	 * make replace pattern of affix
	 */
	if (Affix->type == FF_SUFFIX)
	{
		strcpy(newword, word);
		strcpy(newword + len - Affix->replen, AffixFieldFind(Affix));
		if (baselen)			/* store length of non-changed part of word */
			*baselen = len - Affix->replen;
	}
	else
	{
		/*
		 * if prefix is an all non-changed part's length then all word
		 * contains only prefix and suffix, so out
		 */
		if (baselen && *baselen + Affix->findlen <= Affix->replen)
			return NULL;
		strcpy(newword, AffixFieldFind(Affix));
		strcat(newword, word + Affix->replen);
	}

	/*
	 * check resulting word
	 */
	if (Affix->issimple)
		return newword;
	else if (Affix->isregis)
	{
//		if (RS_execute(&(Affix->reg.regis), newword))
//			return newword;
	}
	else
	{
//		int			err;
//		pg_wchar   *data;
//		size_t		data_len;
//		int			newword_len;

//		/* Convert data string to wide characters */
//		newword_len = strlen(newword);
//		data = (pg_wchar *) palloc((newword_len + 1) * sizeof(pg_wchar));
//		data_len = pg_mb2wchar_with_len(newword, data, newword_len);

//		if (!(err = pg_regexec(&(Affix->reg.regex), data, data_len, 0, NULL, 0, NULL, 0)))
//		{
//			pfree(data);
//			return newword;
//		}
//		pfree(data);
	}

	return NULL;
}

static int
addToResult(char **forms, char **cur, char *word)
{
	if (cur - forms >= MAX_NORM - 1)
		return 0;
	if (forms == cur || strcmp(word, *(cur - 1)) != 0)
	{
		*cur = pstrdup(word);
		*(cur + 1) = NULL;
		return 1;
	}

	return 0;
}

static char **
NormalizeSubWord(IspellDict *Conf, char *word, int flag)
{
	AffixNodeData *suffix = NULL,
			   *prefix = NULL;
	int			slevel = 0,
				plevel = 0;
	int			wrdlen = strlen(word),
				swrdlen;
	char	  **forms;
	char	  **cur;
	char		newword[2 * MAXNORMLEN] = "";
	char		pnewword[2 * MAXNORMLEN] = "";
	AffixNode  *snode = Conf->Suffix,
			   *pnode;
	int			i,
				j;

	if (wrdlen > MAXNORMLEN)
		return NULL;
	cur = forms = (char **) palloc(MAX_NORM * sizeof(char *));
	*cur = NULL;


	/* Check that the word itself is normal form */
	if (FindWord(Conf, word, VoidString, flag))
	{
		*cur = pstrdup(word);
		cur++;
		*cur = NULL;
	}

	/* Find all other NORMAL forms of the 'word' (check only prefix) */
	pnode = Conf->Prefix;
	plevel = 0;
	while (pnode)
	{
		prefix = FindAffixes(pnode, word, wrdlen, &plevel, FF_PREFIX);
		if (!prefix)
			break;
		for (j = 0; j < prefix->naff; j++)
		{
			if (CheckAffix(word, wrdlen, prefix->aff[j], flag, newword, NULL))
			{
				/* prefix success */
				if (FindWord(Conf, newword, AffixFieldFlag(prefix->aff[j]), flag))
					cur += addToResult(forms, cur, newword);
			}
		}
		pnode = prefix->node;
	}

	/*
	 * Find all other NORMAL forms of the 'word' (check suffix and then
	 * prefix)
	 */
	while (snode)
	{
		int			baselen = 0;

		/* find possible suffix */
		suffix = FindAffixes(snode, word, wrdlen, &slevel, FF_SUFFIX);
		if (!suffix)
			break;
		/* foreach suffix check affix */
		for (i = 0; i < suffix->naff; i++)
		{
			if (CheckAffix(word, wrdlen, suffix->aff[i], flag, newword, &baselen))
			{
				/* suffix success */
				if (FindWord(Conf, newword, AffixFieldFlag(suffix->aff[i]), flag))
					cur += addToResult(forms, cur, newword);

				/* now we will look changed word with prefixes */
				pnode = Conf->Prefix;
				plevel = 0;
				swrdlen = strlen(newword);
				while (pnode)
				{
					prefix = FindAffixes(pnode, newword, swrdlen, &plevel, FF_PREFIX);
					if (!prefix)
						break;
					for (j = 0; j < prefix->naff; j++)
					{
						if (CheckAffix(newword, swrdlen, prefix->aff[j], flag, pnewword, &baselen))
						{
							/* prefix success */
							char	   *ff = (prefix->aff[j]->flagflags & suffix->aff[i]->flagflags & FF_CROSSPRODUCT) ?
							VoidString : AffixFieldFlag(prefix->aff[j]);

							if (FindWord(Conf, pnewword, ff, flag))
								cur += addToResult(forms, cur, pnewword);
						}
					}
					pnode = prefix->node;
				}
			}
		}

		snode = suffix->node;
	}

	if (cur == forms)
	{
		pfree(forms);
		return NULL;
	}
	return forms;
}

typedef struct SplitVar
{
	int			nstem;
	int			lenstem;
	char	  **stem;
	struct SplitVar *next;
} SplitVar;

static int
CheckCompoundAffixes(CMPDAffix **ptr, char *word, int len, bool CheckInPlace)
{
	bool		issuffix;

	/* in case CompoundAffix is null: */
	if (*ptr == NULL)
		return -1;

	if (CheckInPlace)
	{
		while ((*ptr)->affix)
		{
			if (len > (*ptr)->len && strncmp((*ptr)->affix, word, (*ptr)->len) == 0)
			{
				len = (*ptr)->len;
				issuffix = (*ptr)->issuffix;
				(*ptr)++;
				return (issuffix) ? len : 0;
			}
			(*ptr)++;
		}
	}
	else
	{
		char	   *affbegin;

		while ((*ptr)->affix)
		{
			if (len > (*ptr)->len && (affbegin = strstr(word, (*ptr)->affix)) != NULL)
			{
				len = (*ptr)->len + (affbegin - word);
				issuffix = (*ptr)->issuffix;
				(*ptr)++;
				return (issuffix) ? len : 0;
			}
			(*ptr)++;
		}
	}
	return -1;
}

static SplitVar *
CopyVar(SplitVar *s, int makedup)
{
	SplitVar   *v = (SplitVar *) palloc(sizeof(SplitVar));

	v->next = NULL;
	if (s)
	{
		int			i;

		v->lenstem = s->lenstem;
		v->stem = (char **) palloc(sizeof(char *) * v->lenstem);
		v->nstem = s->nstem;
		for (i = 0; i < s->nstem; i++)
			v->stem[i] = (makedup) ? pstrdup(s->stem[i]) : s->stem[i];
	}
	else
	{
		v->lenstem = 16;
		v->stem = (char **) palloc(sizeof(char *) * v->lenstem);
		v->nstem = 0;
	}
	return v;
}

static void
AddStem(SplitVar *v, char *word)
{
	if (v->nstem >= v->lenstem)
	{
		v->lenstem *= 2;
		v->stem = (char **) repalloc(v->stem, sizeof(char *) * v->lenstem);
	}

	v->stem[v->nstem] = word;
	v->nstem++;
}

static SplitVar *
SplitToVariants(IspellDict *Conf, SPNode *snode, SplitVar *orig, char *word, int wordlen, int startpos, int minpos)
{
	SplitVar   *var = NULL;
	SPNodeData *StopLow,
			   *StopHigh,
			   *StopMiddle = NULL;
	SPNode	   *node = (snode) ? snode : Conf->Dictionary;
	int			level = (snode) ? minpos : startpos;	/* recursive
														 * minpos==level */
	int			lenaff;
	CMPDAffix  *caff;
	char	   *notprobed;
	int			compoundflag = 0;

	notprobed = (char *) palloc(wordlen);
	memset(notprobed, 1, wordlen);
	var = CopyVar(orig, 1);

	while (level < wordlen)
	{
		/* find word with epenthetic or/and compound affix */
		caff = Conf->CompoundAffix;
		while (level > startpos && (lenaff = CheckCompoundAffixes(&caff, word + level, wordlen - level, (node) ? true : false)) >= 0)
		{
			/*
			 * there is one of compound affixes, so check word for existings
			 */
			char		buf[MAXNORMLEN];
			char	  **subres;

			lenaff = level - startpos + lenaff;

			if (!notprobed[startpos + lenaff - 1])
				continue;

			if (level + lenaff - 1 <= minpos)
				continue;

			if (lenaff >= MAXNORMLEN)
				continue;		/* skip too big value */
			if (lenaff > 0)
				memcpy(buf, word + startpos, lenaff);
			buf[lenaff] = '\0';

			if (level == 0)
				compoundflag = FF_COMPOUNDBEGIN;
			else if (level == wordlen - 1)
				compoundflag = FF_COMPOUNDLAST;
			else
				compoundflag = FF_COMPOUNDMIDDLE;
			subres = NormalizeSubWord(Conf, buf, compoundflag);
			if (subres)
			{
				/* Yes, it was a word from dictionary */
				SplitVar   *new = CopyVar(var, 0);
				SplitVar   *ptr = var;
				char	  **sptr = subres;

				notprobed[startpos + lenaff - 1] = 0;

				while (*sptr)
				{
					AddStem(new, *sptr);
					sptr++;
				}
				pfree(subres);

				while (ptr->next)
					ptr = ptr->next;
				ptr->next = SplitToVariants(Conf, NULL, new, word, wordlen, startpos + lenaff, startpos + lenaff);

				pfree(new->stem);
				pfree(new);
			}
		}

		if (!node)
			break;

		StopLow = node->data;
		StopHigh = node->data + node->length;
		while (StopLow < StopHigh)
		{
			StopMiddle = StopLow + ((StopHigh - StopLow) >> 1);
			if (StopMiddle->val == ((uint8 *) (word))[level])
				break;
			else if (StopMiddle->val < ((uint8 *) (word))[level])
				StopLow = StopMiddle + 1;
			else
				StopHigh = StopMiddle;
		}

		if (StopLow < StopHigh)
		{
			if (startpos == 0)
				compoundflag = FF_COMPOUNDBEGIN;
			else if (level == wordlen - 1)
				compoundflag = FF_COMPOUNDLAST;
			else
				compoundflag = FF_COMPOUNDMIDDLE;

			/* find infinitive */
			if (StopMiddle->isword &&
				(StopMiddle->compoundflag & compoundflag) &&
				notprobed[level])
			{
				/* ok, we found full compoundallowed word */
				if (level > minpos)
				{
					/* and its length more than minimal */
					if (wordlen == level + 1)
					{
						/* well, it was last word */
						AddStem(var, pnstrdup(word + startpos, wordlen - startpos));
						pfree(notprobed);
						return var;
					}
					else
					{
						/* then we will search more big word at the same point */
						SplitVar   *ptr = var;

						while (ptr->next)
							ptr = ptr->next;
						ptr->next = SplitToVariants(Conf, node, var, word, wordlen, startpos, level);
						/* we can find next word */
						level++;
						AddStem(var, pnstrdup(word + startpos, level - startpos));
						node = Conf->Dictionary;
						startpos = level;
						continue;
					}
				}
			}
			node = StopMiddle->node;
		}
		else
			node = NULL;
		level++;
	}

	AddStem(var, pnstrdup(word + startpos, wordlen - startpos));
	pfree(notprobed);
	return var;
}

static void
addNorm(TSLexeme **lres, TSLexeme **lcur, char *word, int flags, uint16 NVariant)
{
	if (*lres == NULL)
		*lcur = *lres = (TSLexeme *) palloc(MAX_NORM * sizeof(TSLexeme));

	if (*lcur - *lres < MAX_NORM - 1)
	{
		(*lcur)->lexeme = word;
		(*lcur)->flags = flags;
		(*lcur)->nvariant = NVariant;
		(*lcur)++;
		(*lcur)->lexeme = NULL;
	}
}

TSLexeme *
NINormalizeWord(IspellDict *Conf, char *word)
{
	char	  **res;
	TSLexeme   *lcur = NULL,
			   *lres = NULL;
	uint16		NVariant = 1;

	res = NormalizeSubWord(Conf, word, 0);

	if (res)
	{
		char	  **ptr = res;

		while (*ptr && (lcur - lres) < MAX_NORM)
		{
			addNorm(&lres, &lcur, *ptr, 0, NVariant++);
			ptr++;
		}
		pfree(res);
	}

	if (Conf->usecompound)
	{
		int			wordlen = strlen(word);
		SplitVar   *ptr,
				   *var = SplitToVariants(Conf, NULL, NULL, word, wordlen, 0, -1);
		int			i;

		while (var)
		{
			if (var->nstem > 1)
			{
				char	  **subres = NormalizeSubWord(Conf, var->stem[var->nstem - 1], FF_COMPOUNDLAST);

				if (subres)
				{
					char	  **subptr = subres;

					while (*subptr)
					{
						for (i = 0; i < var->nstem - 1; i++)
						{
							addNorm(&lres, &lcur, (subptr == subres) ? var->stem[i] : pstrdup(var->stem[i]), 0, NVariant);
						}

						addNorm(&lres, &lcur, *subptr, 0, NVariant);
						subptr++;
						NVariant++;
					}

					pfree(subres);
					var->stem[0] = NULL;
					pfree(var->stem[var->nstem - 1]);
				}
			}

			for (i = 0; i < var->nstem && var->stem[i]; i++)
				pfree(var->stem[i]);
			ptr = var->next;
			pfree(var->stem);
			pfree(var);
			var = ptr;
		}
	}

	return lres;
}
