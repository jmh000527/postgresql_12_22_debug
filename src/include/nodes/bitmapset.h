/*-------------------------------------------------------------------------
 *
 * bitmapset.h
 *	  PostgreSQL generic bitmap set package
 *
 * A bitmap set can represent any set of nonnegative integers, although
 * it is mainly intended for sets where the maximum value is not large,
 * say at most a few hundred.  By convention, a NULL pointer is always
 * accepted by all operations to represent the empty set.  (But beware
 * that this is not the only representation of the empty set.  Use
 * bms_is_empty() in preference to testing for NULL.)
 *
 *
 * Copyright (c) 2003-2019, PostgreSQL Global Development Group
 *
 * src/include/nodes/bitmapset.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef BITMAPSET_H
#define BITMAPSET_H

/*
 * Forward decl to save including pg_list.h
 */
struct List;

/*
 * Data representation
 *
 * Larger bitmap word sizes generally give better performance, so long as
 * they're not wider than the processor can handle efficiently.  We use
 * 64-bit words if pointers are that large, else 32-bit words.
 */
#if SIZEOF_VOID_P >= 8

#define BITS_PER_BITMAPWORD 64
typedef uint64 bitmapword;		/* must be an unsigned type */
typedef int64 signedbitmapword; /* must be the matching signed type */

#else

#define BITS_PER_BITMAPWORD 32
typedef uint32 bitmapword;		/* must be an unsigned type */
typedef int32 signedbitmapword; /* must be the matching signed type */

#endif

// 位图集合结构体
typedef struct Bitmapset
{
	int			nwords;			// 数组中的字数
	bitmapword	words[FLEXIBLE_ARRAY_MEMBER];	// 实际长度为 [nwords] 的位图数组
} Bitmapset;


// bms_subset_compare 的结果类型
typedef enum
{
	BMS_EQUAL,					// 两个集合相等
	BMS_SUBSET1,				// 第一个集合是第二个集合的子集
	BMS_SUBSET2,				// 第二个集合是第一个集合的子集
	BMS_DIFFERENT				// 两个集合互不为子集
} BMS_Comparison;

// bms_membership 的结果类型
typedef enum
{
	BMS_EMPTY_SET,				// 空集合（0 个成员）
	BMS_SINGLETON,				// 单元素集合（1 个成员）
	BMS_MULTIPLE				// 多元素集合（>1 个成员）
} BMS_Membership;


/*
 * function prototypes in nodes/bitmapset.c
 */

extern Bitmapset *bms_copy(const Bitmapset *a);
extern bool bms_equal(const Bitmapset *a, const Bitmapset *b);
extern int	bms_compare(const Bitmapset *a, const Bitmapset *b);
extern Bitmapset *bms_make_singleton(int x);
extern void bms_free(Bitmapset *a);

extern Bitmapset *bms_union(const Bitmapset *a, const Bitmapset *b);
extern Bitmapset *bms_intersect(const Bitmapset *a, const Bitmapset *b);
extern Bitmapset *bms_difference(const Bitmapset *a, const Bitmapset *b);
extern bool bms_is_subset(const Bitmapset *a, const Bitmapset *b);
extern BMS_Comparison bms_subset_compare(const Bitmapset *a, const Bitmapset *b);
extern bool bms_is_member(int x, const Bitmapset *a);
extern int	bms_member_index(Bitmapset *a, int x);
extern bool bms_overlap(const Bitmapset *a, const Bitmapset *b);
extern bool bms_overlap_list(const Bitmapset *a, const struct List *b);
extern bool bms_nonempty_difference(const Bitmapset *a, const Bitmapset *b);
extern int	bms_singleton_member(const Bitmapset *a);
extern bool bms_get_singleton_member(const Bitmapset *a, int *member);
extern int	bms_num_members(const Bitmapset *a);

/* optimized tests when we don't need to know exact membership count: */
extern BMS_Membership bms_membership(const Bitmapset *a);
extern bool bms_is_empty(const Bitmapset *a);

/* these routines recycle (modify or free) their non-const inputs: */

extern Bitmapset *bms_add_member(Bitmapset *a, int x);
extern Bitmapset *bms_del_member(Bitmapset *a, int x);
extern Bitmapset *bms_add_members(Bitmapset *a, const Bitmapset *b);
extern Bitmapset *bms_add_range(Bitmapset *a, int lower, int upper);
extern Bitmapset *bms_int_members(Bitmapset *a, const Bitmapset *b);
extern Bitmapset *bms_del_members(Bitmapset *a, const Bitmapset *b);
extern Bitmapset *bms_join(Bitmapset *a, Bitmapset *b);

/* support for iterating through the integer elements of a set: */
extern int	bms_first_member(Bitmapset *a);
extern int	bms_next_member(const Bitmapset *a, int prevbit);
extern int	bms_prev_member(const Bitmapset *a, int prevbit);

/* support for hashtables using Bitmapsets as keys: */
extern uint32 bms_hash_value(const Bitmapset *a);

#endif							/* BITMAPSET_H */
