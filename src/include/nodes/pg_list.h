/*-------------------------------------------------------------------------
 *
 * pg_list.h
 *	  interface for PostgreSQL generic linked list package
 *
 * This package implements singly-linked homogeneous lists.
 *
 * It is important to have constant-time length, append, and prepend
 * operations. To achieve this, we deal with two distinct data
 * structures:
 *
 *		1. A set of "list cells": each cell contains a data field and
 *		   a link to the next cell in the list or NULL.
 *		2. A single structure containing metadata about the list: the
 *		   type of the list, pointers to the head and tail cells, and
 *		   the length of the list.
 *
 * We support three types of lists:
 *
 *	T_List: lists of pointers
 *		(in practice usually pointers to Nodes, but not always;
 *		declared as "void *" to minimize casting annoyances)
 *	T_IntList: lists of integers
 *	T_OidList: lists of Oids
 *
 * (At the moment, ints and Oids are the same size, but they may not
 * always be so; try to be careful to maintain the distinction.)
 *
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/nodes/pg_list.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_LIST_H
#define PG_LIST_H

#include "nodes/nodes.h"


 /* 前向声明：ListCell 结构体，用于在 List 中作为节点 */
typedef struct ListCell ListCell;

/* 列表元数据结构：包含类型、长度、头尾指针 */
typedef struct List
{
	NodeTag		type;			/* T_List, T_IntList, or T_OidList */
	int			length;			/* 列表长度（常量时间获取） */
	ListCell* head;			/* 指向第一个元素的指针（非空列表保证不为 NULL） */
	ListCell* tail;			/* 指向最后一个元素的指针（用于常量时间追加） */
} List;

/* 列表节点：保存数据（指针/整数/Oid 之一）和指向下一个节点的指针 */
struct ListCell
{
	union
	{
		void* ptr_value;	/* 指针类型数据 */
		int			int_value;	/* 整数类型数据 */
		Oid			oid_value;	/* Oid 类型数据 */
	}			data;
	ListCell* next;			/* 指向下一个 ListCell，链表终点为 NULL */
};

/*
 * 空列表的唯一合法表示是 NIL（即 NULL 指针）。
 * 换言之，非 NIL 的列表保证 length >= 1 且 head/tail != NULL。
 */
#define NIL						((List *) NULL)

 /*
  * 下面这些访问例程使用内联函数实现，而不是宏。
  * 原因是它们调用频繁，但要避免宏带来的参数重复求值问题。
  */

  /* 返回列表的头结点（若列表为 NIL 则返回 NULL） */
static inline ListCell*
list_head(const List* l)
{
	return l ? l->head : NULL;
}

/* 返回列表的尾结点（若列表为 NIL 则返回 NULL） */
static inline ListCell*
list_tail(List* l)
{
	return l ? l->tail : NULL;
}

/* 返回列表长度（若列表为 NIL 则返回 0） */
static inline int
list_length(const List* l)
{
	return l ? l->length : 0;
}

/*
 * NB: There is an unfortunate legacy from a previous incarnation of
 * the List API: the macro lfirst() was used to mean "the data in this
 * cons cell". To avoid changing every usage of lfirst(), that meaning
 * has been kept. As a result, lfirst() takes a ListCell and returns
 * the data it contains; to get the data in the first cell of a
 * List, use linitial(). Worse, lsecond() is more closely related to
 * linitial() than lfirst(): given a List, lsecond() returns the data
 * in the second cons cell.
 *
 * 注意：List API 在先前的实现中遗留了一个不太理想的习惯：宏 lfirst() 被用来表示“该 cons 单元中的数据”。
 * 为了避免修改每处对 lfirst() 的使用，这个含义被保留。因此 lfirst() 接受一个 ListCell 并返回其包含的数据；
 * 若要获取一个 List 的第一个单元的数据，应使用 linitial()。
 * 更进一步，lsecond() 的语义更接近 linitial() 而不是 lfirst()：对于一个 List，lsecond() 返回第二个 cons 单元中的数据。
 */

#define lnext(lc)				((lc)->next)			/* 返回下一个 ListCell 指针 */
#define lfirst(lc)				((lc)->data.ptr_value)	/* 返回节点中指针类型数据 */
#define lfirst_int(lc)			((lc)->data.int_value)	/* 返回节点中整型数据 */
#define lfirst_oid(lc)			((lc)->data.oid_value)	/* 返回节点中 Oid 类型数据 */
#define lfirst_node(type,lc)	castNode(type, lfirst(lc))/* 将指针数据按 node 类型转换并返回 */

#define linitial(l)				lfirst(list_head(l))			/* 返回列表首元素的数据（指针） */
#define linitial_int(l)			lfirst_int(list_head(l))		/* 返回列表首元素的整型数据 */
#define linitial_oid(l)			lfirst_oid(list_head(l))		/* 返回列表首元素的 Oid 数据 */
#define linitial_node(type,l)	castNode(type, linitial(l))		/* 返回首元素并按 type 转换 */

#define lsecond(l)				lfirst(lnext(list_head(l)))			/* 返回列表第二个元素的数据 */
#define lsecond_int(l)			lfirst_int(lnext(list_head(l)))		/* 返回列表第二个元素的整型数据 */
#define lsecond_oid(l)			lfirst_oid(lnext(list_head(l)))		/* 返回列表第二个元素的 Oid 数据 */
#define lsecond_node(type,l)	castNode(type, lsecond(l))			/* 返回第二个元素并按 type 转换 */

#define lthird(l)				lfirst(lnext(lnext(list_head(l))))			/* 返回第三个元素的数据 */
#define lthird_int(l)			lfirst_int(lnext(lnext(list_head(l))))	/* 返回第三个元素的整型数据 */
#define lthird_oid(l)			lfirst_oid(lnext(lnext(list_head(l))))	/* 返回第三个元素的 Oid 数据 */
#define lthird_node(type,l)		castNode(type, lthird(l))					/* 返回第三个元素并按 type 转换 */

#define lfourth(l)				lfirst(lnext(lnext(lnext(list_head(l)))))			/* 返回第四个元素的数据 */
#define lfourth_int(l)			lfirst_int(lnext(lnext(lnext(list_head(l)))))		/* 返回第四个元素的整型数据 */
#define lfourth_oid(l)			lfirst_oid(lnext(lnext(lnext(list_head(l)))))		/* 返回第四个元素的 Oid 数据 */
#define lfourth_node(type,l)	castNode(type, lfourth(l))							/* 返回第四个元素并按 type 转换 */

#define llast(l)				lfirst(list_tail(l))			/* 返回列表最后一个元素的数据（指针） */
#define llast_int(l)			lfirst_int(list_tail(l))		/* 返回列表最后一个元素的整型数据 */
#define llast_oid(l)			lfirst_oid(list_tail(l))		/* 返回列表最后一个元素的 Oid 数据 */
#define llast_node(type,l)		castNode(type, llast(l))		/* 返回最后一个元素并按 type 转换 */

 /*
  * Convenience macros for building fixed-length lists
  */
#define list_make1(x1)				lcons(x1, NIL)						/* 构造仅含 1 元素的列表 */
#define list_make2(x1,x2)			lcons(x1, list_make1(x2))			/* 构造 2 元素列表 */
#define list_make3(x1,x2,x3)		lcons(x1, list_make2(x2, x3))		/* 构造 3 元素列表 */
#define list_make4(x1,x2,x3,x4)		lcons(x1, list_make3(x2, x3, x4))	/* 构造 4 元素列表 */
#define list_make5(x1,x2,x3,x4,x5)	lcons(x1, list_make4(x2, x3, x4, x5))/* 构造 5 元素列表 */

#define list_make1_int(x1)			lcons_int(x1, NIL)						/* 构造仅含 1 个 int 的列表 */
#define list_make2_int(x1,x2)		lcons_int(x1, list_make1_int(x2))		/* 构造 2 个 int 的列表 */
#define list_make3_int(x1,x2,x3)	lcons_int(x1, list_make2_int(x2, x3))	/* 构造 3 个 int 的列表 */
#define list_make4_int(x1,x2,x3,x4) lcons_int(x1, list_make3_int(x2, x3, x4))	/* 构造 4 个 int 的列表 */
#define list_make5_int(x1,x2,x3,x4,x5)	lcons_int(x1, list_make4_int(x2, x3, x4, x5))/* 构造 5 个 int 的列表 */

#define list_make1_oid(x1)			lcons_oid(x1, NIL)						/* 构造仅含 1 个 Oid 的列表 */
#define list_make2_oid(x1,x2)		lcons_oid(x1, list_make1_oid(x2))		/* 构造 2 个 Oid 的列表 */
#define list_make3_oid(x1,x2,x3)	lcons_oid(x1, list_make2_oid(x2, x3))	/* 构造 3 个 Oid 的列表 */
#define list_make4_oid(x1,x2,x3,x4) lcons_oid(x1, list_make3_oid(x2, x3, x4))	/* 构造 4 个 Oid 的列表 */
#define list_make5_oid(x1,x2,x3,x4,x5)	lcons_oid(x1, list_make4_oid(x2, x3, x4, x5))/* 构造 5 个 Oid 的列表 */

/*
 * foreach -
 *	  一个便利宏，用于遍历整个列表
 */
#define foreach(cell, l)	\
	for ((cell) = list_head(l); (cell) != NULL; (cell) = lnext(cell))	/* 遍历列表的常用宏：cell 为 ListCell 指针 */

/*
 * for_each_cell -
 *	  一个便利宏，用于从指定节点开始遍历列表
 */
#define for_each_cell(cell, initcell)	\
	for ((cell) = (initcell); (cell) != NULL; (cell) = lnext(cell))	/* 从指定节点开始遍历 */

/*
 * forboth -
 *	  a convenience macro for advancing through two linked lists
 *	  simultaneously. This macro loops through both lists at the same
 *	  time, stopping when either list runs out of elements. Depending
 *	  on the requirements of the call site, it may also be wise to
 *	  assert that the lengths of the two lists are equal.
 */
#define forboth(cell1, list1, cell2, list2)							\
	for ((cell1) = list_head(list1), (cell2) = list_head(list2);	\
		 (cell1) != NULL && (cell2) != NULL;						\
		 (cell1) = lnext(cell1), (cell2) = lnext(cell2))			/* 同时遍历两个列表，直到任一结束 */

/*
 * for_both_cell -
 *	  a convenience macro which loops through two lists starting from the
 *	  specified cells of each. This macro loops through both lists at the same
 *	  time, stopping when either list runs out of elements.  Depending on the
 *	  requirements of the call site, it may also be wise to assert that the
 *	  lengths of the two lists are equal, and initcell1 and initcell2 are at
 *	  the same position in the respective lists.
 */
#define for_both_cell(cell1, initcell1, cell2, initcell2)	\
	for ((cell1) = (initcell1), (cell2) = (initcell2);		\
		 (cell1) != NULL && (cell2) != NULL;				\
		 (cell1) = lnext(cell1), (cell2) = lnext(cell2))	/* 从各自指定起点同时遍历两个列表 */

/*
 * forthree -
 *	  the same for three lists
 */
#define forthree(cell1, list1, cell2, list2, cell3, list3)			\
	for ((cell1) = list_head(list1), (cell2) = list_head(list2), (cell3) = list_head(list3); \
		 (cell1) != NULL && (cell2) != NULL && (cell3) != NULL;		\
		 (cell1) = lnext(cell1), (cell2) = lnext(cell2), (cell3) = lnext(cell3)) /* 同时遍历三个列表 */

/*
 * forfour -
 *	  the same for four lists
 */
#define forfour(cell1, list1, cell2, list2, cell3, list3, cell4, list4) \
	for ((cell1) = list_head(list1), (cell2) = list_head(list2), \
		 (cell3) = list_head(list3), (cell4) = list_head(list4); \
		 (cell1) != NULL && (cell2) != NULL && \
		 (cell3) != NULL && (cell4) != NULL; \
		 (cell1) = lnext(cell1), (cell2) = lnext(cell2), \
		 (cell3) = lnext(cell3), (cell4) = lnext(cell4)) /* 同时遍历四个列表 */

/*
 * forfive -
 *	  the same for five lists
 */
#define forfive(cell1, list1, cell2, list2, cell3, list3, cell4, list4, cell5, list5) \
	for ((cell1) = list_head(list1), (cell2) = list_head(list2), \
		 (cell3) = list_head(list3), (cell4) = list_head(list4), \
		 (cell5) = list_head(list5); \
		 (cell1) != NULL && (cell2) != NULL && (cell3) != NULL && \
		 (cell4) != NULL && (cell5) != NULL; \
		 (cell1) = lnext(cell1), (cell2) = lnext(cell2), \
		 (cell3) = lnext(cell3), (cell4) = lnext(cell4), \
		 (cell5) = lnext(cell5)) /* 同时遍历五个列表 */

extern List* lappend(List* list, void* datum);						/* 在列表末尾追加指针类型数据 */
extern List* lappend_int(List* list, int datum);						/* 在列表末尾追加整型数据 */
extern List* lappend_oid(List* list, Oid datum);						/* 在列表末尾追加 Oid 数据 */

extern ListCell* lappend_cell(List* list, ListCell* prev, void* datum);			/* 在 prev 之后插入新 cell（指针） */
extern ListCell* lappend_cell_int(List* list, ListCell* prev, int datum);		/* 在 prev 之后插入新 cell（int） */
extern ListCell* lappend_cell_oid(List* list, ListCell* prev, Oid datum);		/* 在 prev 之后插入新 cell（Oid） */

extern List* lcons(void* datum, List* list);				/* 在列表头部插入指针类型数据 */
extern List* lcons_int(int datum, List* list);				/* 在列表头部插入整型数据 */
extern List* lcons_oid(Oid datum, List* list);				/* 在列表头部插入 Oid 数据 */

extern List* list_concat(List* list1, List* list2);			/* 连接两个列表（破坏 list1 或 list2 取决于实现） */
extern List* list_truncate(List* list, int new_size);			/* 截断列表到指定长度 */

extern ListCell* list_nth_cell(const List* list, int n);		/* 返回第 n 个 ListCell（0-based） */
extern void* list_nth(const List* list, int n);				/* 返回第 n 个元素的指针数据 */
extern int	list_nth_int(const List* list, int n);			/* 返回第 n 个元素的整型数据 */
extern Oid	list_nth_oid(const List* list, int n);			/* 返回第 n 个元素的 Oid 数据 */
#define list_nth_node(type,list,n)	castNode(type, list_nth(list, n))	/* 返回第 n 个元素并按 type 转换 */

extern bool list_member(const List* list, const void* datum);	/* 判断指针数据是否为列表成员（使用 equal） */
extern bool list_member_ptr(const List* list, const void* datum);/* 使用指针比较判断成员关系 */
extern bool list_member_int(const List* list, int datum);		/* 判断整型数据是否为列表成员 */
extern bool list_member_oid(const List* list, Oid datum);		/* 判断 Oid 是否为列表成员 */

extern List* list_delete(List* list, void* datum);				/* 删除匹配 equal() 的首个元素（指针比较或 equal） */
extern List* list_delete_ptr(List* list, void* datum);			/* 使用指针比较删除首个匹配元素 */
extern List* list_delete_int(List* list, int datum);			/* 删除匹配的整型元素 */
extern List* list_delete_oid(List* list, Oid datum);			/* 删除匹配的 Oid 元素 */
extern List* list_delete_first(List* list);						/* 删除第一个元素 */
extern List* list_delete_cell(List* list, ListCell* cell, ListCell* prev); /* 删除指定 cell（需提供前驱） */

extern List* list_union(const List* list1, const List* list2);			/* 返回两个列表的并集（使用 equal） */
extern List* list_union_ptr(const List* list1, const List* list2);		/* 使用指针比较的并集 */
extern List* list_union_int(const List* list1, const List* list2);		/* 整型并集 */
extern List* list_union_oid(const List* list1, const List* list2);		/* Oid 并集 */

extern List* list_intersection(const List* list1, const List* list2);		/* 返回两个列表的交集（使用 equal） */
extern List* list_intersection_int(const List* list1, const List* list2);	/* 整型交集 */

/* currently, there's no need for list_intersection_ptr etc */

extern List* list_difference(const List* list1, const List* list2);			/* 列表差集：list1 - list2（使用 equal） */
extern List* list_difference_ptr(const List* list1, const List* list2);		/* 使用指针比较的差集 */
extern List* list_difference_int(const List* list1, const List* list2);		/* 整型差集 */
extern List* list_difference_oid(const List* list1, const List* list2);		/* Oid 差集 */

extern List* list_append_unique(List* list, void* datum);			/* 若不在列表中则追加（使用 equal） */
extern List* list_append_unique_ptr(List* list, void* datum);		/* 使用指针比较的 append_unique */
extern List* list_append_unique_int(List* list, int datum);			/* 整型 append_unique */
extern List* list_append_unique_oid(List* list, Oid datum);			/* Oid append_unique */

extern List* list_concat_unique(List* list1, List* list2);			/* 将 list2 中不在 list1 的元素追加到 list1（使用 equal） */
extern List* list_concat_unique_ptr(List* list1, List* list2);		/* 使用指针比较的 concat_unique */
extern List* list_concat_unique_int(List* list1, List* list2);		/* 整型 concat_unique */
extern List* list_concat_unique_oid(List* list1, List* list2);		/* Oid concat_unique */

extern void list_free(List* list);				/* 释放列表结构（不释放元素本身） */
extern void list_free_deep(List* list);			/* 深度释放列表：释放元素指向的数据以及列表结构 */

extern List* list_copy(const List* list);			/* 浅拷贝列表结构（复制cell但不复制数据） */
extern List* list_copy_tail(const List* list, int nskip);	/* 复制从第 nskip 个之后的子列表 */

typedef int (*list_qsort_comparator) (const void* a, const void* b);
extern List* list_qsort(const List* list, list_qsort_comparator cmp);	/* 对列表进行排序，返回新列表 */

/*
 * To ease migration to the new list API, a set of compatibility
 * macros are provided that reduce the impact of the list API changes
 * as far as possible. Until client code has been rewritten to use the
 * new list API, the ENABLE_LIST_COMPAT symbol can be defined before
 * including pg_list.h
 */
#ifdef ENABLE_LIST_COMPAT

#define lfirsti(lc)					lfirst_int(lc)
#define lfirsto(lc)					lfirst_oid(lc)

#define makeList1(x1)				list_make1(x1)
#define makeList2(x1, x2)			list_make2(x1, x2)
#define makeList3(x1, x2, x3)		list_make3(x1, x2, x3)
#define makeList4(x1, x2, x3, x4)	list_make4(x1, x2, x3, x4)

#define makeListi1(x1)				list_make1_int(x1)
#define makeListi2(x1, x2)			list_make2_int(x1, x2)

#define makeListo1(x1)				list_make1_oid(x1)
#define makeListo2(x1, x2)			list_make2_oid(x1, x2)

#define lconsi(datum, list)			lcons_int(datum, list)
#define lconso(datum, list)			lcons_oid(datum, list)

#define lappendi(list, datum)		lappend_int(list, datum)
#define lappendo(list, datum)		lappend_oid(list, datum)

#define nconc(l1, l2)				list_concat(l1, l2)

#define nth(n, list)				list_nth(list, n)

#define member(datum, list)			list_member(list, datum)
#define ptrMember(datum, list)		list_member_ptr(list, datum)
#define intMember(datum, list)		list_member_int(list, datum)
#define oidMember(datum, list)		list_member_oid(list, datum)

 /*
  * Note that the old lremove() determined equality via pointer
  * comparison, whereas the new list_delete() uses equal(); in order to
  * keep the same behavior, we therefore need to map lremove() calls to
  * list_delete_ptr() rather than list_delete()
  */
#define lremove(elem, list)			list_delete_ptr(list, elem)
#define LispRemove(elem, list)		list_delete(list, elem)
#define lremovei(elem, list)		list_delete_int(list, elem)
#define lremoveo(elem, list)		list_delete_oid(list, elem)

#define ltruncate(n, list)			list_truncate(list, n)

#define set_union(l1, l2)			list_union(l1, l2)
#define set_uniono(l1, l2)			list_union_oid(l1, l2)
#define set_ptrUnion(l1, l2)		list_union_ptr(l1, l2)

#define set_difference(l1, l2)		list_difference(l1, l2)
#define set_differenceo(l1, l2)		list_difference_oid(l1, l2)
#define set_ptrDifference(l1, l2)	list_difference_ptr(l1, l2)

#define equali(l1, l2)				equal(l1, l2)
#define equalo(l1, l2)				equal(l1, l2)

#define freeList(list)				list_free(list)

#define listCopy(list)				list_copy(list)

extern int	length(List* list);
#endif							/* ENABLE_LIST_COMPAT */

#endif							/* PG_LIST_H */
