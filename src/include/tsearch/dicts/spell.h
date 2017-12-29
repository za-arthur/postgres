/*-------------------------------------------------------------------------
 *
 * spell.h
 *
 * Declarations for ISpell dictionary
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 *
 * src/include/tsearch/dicts/spell.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef __SPELL_H__
#define __SPELL_H__

#include "regex/regex.h"
#include "tsearch/dicts/regis.h"
#include "tsearch/ts_public.h"

#define ISPELL_INVALID_INDEX	(-1)
#define ISPELL_INVALID_OFFSET	(0xFFFFFFFF)

/*
 * SPNode and SPNodeData are used to represent prefix tree (Trie) to store
 * a words list.
 */
struct SPNode;

typedef struct
{
	uint32		val:8,
				isword:1,
	/* Stores compound flags listed below */
				compoundflag:4,
	/* Index of an entry of the AffixData field */
				affix:19;
	/* Offset to a node of the DictNodes field */
	uint32		node_offset;
} SPNodeData;

/*
 * Names of FF_ are correlated with Hunspell options in affix file
 * http://hunspell.sourceforge.net/
 */
#define FF_COMPOUNDONLY		0x01
#define FF_COMPOUNDBEGIN	0x02
#define FF_COMPOUNDMIDDLE	0x04
#define FF_COMPOUNDLAST		0x08
#define FF_COMPOUNDFLAG		( FF_COMPOUNDBEGIN | FF_COMPOUNDMIDDLE | \
							FF_COMPOUNDLAST )
#define FF_COMPOUNDFLAGMASK		0x0f

typedef struct SPNode
{
	uint32		length;
	SPNodeData	data[FLEXIBLE_ARRAY_MEMBER];
} SPNode;

#define SPNHDRSZ	(offsetof(SPNode,data))

/*
 * Represents an entry in a words list.
 */
typedef struct spell_struct
{
	union
	{
		/*
		 * flag is filled in by NIImportDictionary(). After
		 * NISortDictionary(), d is used instead of flag.
		 */
		char	   *flag;
		/* d is used in mkSPNode() */
		struct
		{
			/* Reference to an entry of the AffixData field */
			int			affix;
			/* Length of the word */
			int			len;
		}			d;
	}			p;
	char		word[FLEXIBLE_ARRAY_MEMBER];
} SPELL;

#define SPELLHDRSZ	(offsetof(SPELL, word))

/*
 * Represents an entry in an affix list.
 */
typedef struct aff_struct
{
	/* FF_SUFFIX or FF_PREFIX */
	uint16		type:1,
				flagflags:7,
				issimple:1,
				isregis:1,
				flaglen:2;

	/* 8 bytes could be too mach for repl and find, but who knows */
	uint8		replen;
	uint8		findlen;

	/*
	 * fields stores the following data (each ends with \0):
	 * - repl
	 * - find
	 * - flag - one character (if FM_CHAR),
	 *          two characters (if FM_LONG),
	 *          number, >= 0 and < 65536 (if FM_NUM).
	 */
	char		fields[FLEXIBLE_ARRAY_MEMBER];
} AFFIX;

#define AF_FLAG_MAXSIZE		5		/* strlen(65536) */
#define AF_REPL_MAXSIZE		255		/* 8 bytes */
#define AF_FIND_MAXSIZE		255		/* 8 bytes */

#define AFFIXHDRSZ	(offsetof(AFFIX, fields))

#define AffixFieldRepl(af)	((af)->fields)
#define AffixFieldFind(af)	((af)->fields + (af)->replen + 1)
#define AffixFieldFlag(af)	(AffixFieldFind(af) + (af)->findlen + 1)
//#define AffixSize(af)		(AFFIXHDRSZ + strlen((af)->fields) + 1)

/*
 * affixes use dictionary flags too
 */
#define FF_COMPOUNDPERMITFLAG	0x10
#define FF_COMPOUNDFORBIDFLAG	0x20
#define FF_CROSSPRODUCT			0x40

/*
 * Don't change the order of these. Initialization sorts by these,
 * and expects prefixes to come first after sorting.
 */
#define FF_SUFFIX				1
#define FF_PREFIX				0

/*
 * AffixNode and AffixNodeData are used to represent prefix tree (Trie) to store
 * an affix list.
 */
struct AffixNode;

typedef struct
{
	uint32		val:8,
				naff:24;
	AFFIX	  **aff;
	struct AffixNode *node;
} AffixNodeData;

typedef struct AffixNode
{
	uint32		isvoid:1,
				length:31;
	AffixNodeData data[FLEXIBLE_ARRAY_MEMBER];
} AffixNode;

#define ANHRDSZ		   (offsetof(AffixNode, data))

typedef struct
{
	/* Index of an affix of the Affix field */
	uint32		affix;
	int			len;
	bool		issuffix;
} CMPDAffix;

/*
 * Type of encoding affix flags in Hunspell dictionaries
 */
typedef enum
{
	FM_CHAR,					/* one character (like ispell) */
	FM_LONG,					/* two characters */
	FM_NUM						/* number, >= 0 and < 65536 */
} FlagMode;

/*
 * Structure to store Hunspell options. Flag representation depends on flag
 * type. These flags are about support of compound words.
 */
typedef struct CompoundAffixFlag
{
	union
	{
		/* Flag name if flagMode is FM_CHAR or FM_LONG */
		char	   *s;
		/* Flag name if flagMode is FM_NUM */
		uint32		i;
	}			flag;
	/* we don't have a bsearch_arg version, so, copy FlagMode */
	FlagMode	flagMode;
	uint32		value;
} CompoundAffixFlag;

#define FLAGNUM_MAXSIZE		(1 << 16)

/*
 * IspellDictBuild is used to initialize IspellDict struct.  This is a
 * temprorary structure which is setup by NIStartBuild() and released by
 * NIFinishBuild().
 */
typedef struct IspellDictBuild
{
	MemoryContext buildCxt;		/* temp context for construction */

	IspellDictData *dict;
	uint32		dict_size;

	/* Temporary data */

	/* Array of Hunspell options in affix file */
	CompoundAffixFlag *CompoundAffixFlags;
	/* number of entries in CompoundAffixFlags array */
	int			nCompoundAffixFlag;
	/* allocated length of CompoundAffixFlags array */
	int			mCompoundAffixFlag;

	/* Array of all words in the dict file */
	SPELL	  **Spell;
	int			nSpell;			/* number of valid entries in Spell array */
	int			mSpell;			/* allocated length of Spell array */

	/* Data for IspellDictData */

	/* Array of all affixes in the aff file */
	uint32	   *AffixOffset;
	int			nAffix;			/* number of valid entries in Affix array */
	int			mAffix;			/* allocated length of Affix array */
	char	   *Affix;
	uint32		AffixSize;		/* allocated size of Affix */
	uint32		AffixEnd;		/* end of data in Affix */

	/* Array of sets of affixes */
	uint32	   *AffixDataOffset;
	int			nAffixData;		/* number of affix sets */
	int			mAffixData;		/* allocated number of affix sets */
	char	   *AffixData;
	uint32		AffixDataSize;	/* allocated size of AffixData */
	uint32		AffixDataEnd;	/* end of data in AffixData */

	/* Prefix tree which stores a word list */
	char	   *DictNodes;
	uint32		DictNodesSize;	/* allocated size of DictNodes */
	uint32		DictNodesEnd;	/* end of data in DictNodes */

	/* Array of compound affixes */
	CMPDAffix  *CompoundAffix;
} IspellDictBuild;

#define AffixGet(d, i)			(((i) == ISPELL_INVALID_INDEX) ? NULL : (d)->Affix + (d)->AffixOffset[i])
#define AffixDataGet(d, i)		((d)->AffixData + (d)->AffixDataOffset[i])
#define DictNodeGet(d, of)		(((of) == ISPELL_INVALID_OFFSET) ? NULL : (d)->DictNodes + (of))

typedef struct IspellDictData
{
	FlagMode	flagMode;
	bool		usecompound;

	bool		useFlagAliases;

	uint32		AffixDataSize;
	/*
	 * data stores:
	 * - array of sets of affixes
	 * - array of AFFIX
	 */
	char		data[FLEXIBLE_ARRAY_MEMBER];
} IspellDictData;

#define IspellDictDataHdrSize	(offsetof(IspellDictData, data))

typedef struct
{
	/* Allocated size of IspellDict */
	uint32		size;

	AffixNode  *Suffix;
	AffixNode  *Prefix;

	SPNode	   *Dictionary;
	/* Array of sets of affixes */
	char	  **AffixData;
	int			lenAffixData;
	int			nAffixData;
	bool		useFlagAliases;

	CMPDAffix  *CompoundAffix;

	bool		usecompound;
	FlagMode	flagMode;

	/* These are used to allocate "compact" data without palloc overhead */
	char	   *firstfree;		/* first free address (always maxaligned) */
	size_t		avail;			/* free space remaining at firstfree */

	/*
	 * data stores:
	 * - array of sets of affixes
	 * - array of AFFIX
	 */
	char		data[FLEXIBLE_ARRAY_MEMBER];
} IspellDict;

#define IspellDictHdrSize	(offsetof(IspellDict, data))

extern TSLexeme *NINormalizeWord(IspellDict *Conf, char *word);

extern void NIStartBuild(IspellDictBuild *ConfBuild);
extern void NIImportAffixes(IspellDictBuild *ConfBuild, const char *filename);
extern void NIImportDictionary(IspellDictBuild *ConfBuild,
							   const char *filename);
extern void NISortDictionary(IspellDictBuild *ConfBuild);
extern void NISortAffixes(IspellDictBuild *ConfBuild);
extern void NIFinishBuild(IspellDictBuild *ConfBuild);

#endif
