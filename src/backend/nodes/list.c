/*-------------------------------------------------------------------------
 *
 * list.c
 *	  implementation for PostgreSQL generic linked list package
 *
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/nodes/list.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "nodes/pg_list.h"


/*
 * Routines to simplify writing assertions about the type of a list; a
 * NIL list is considered to be an empty list of any type.
 */
#define IsPointerList(l)		((l) == NIL || IsA((l), List))
#define IsIntegerList(l)		((l) == NIL || IsA((l), IntList))
#define IsOidList(l)			((l) == NIL || IsA((l), OidList))

#ifdef USE_ASSERT_CHECKING
/*
 * Check that the specified List is valid (so far as we can tell).
 */
static void
check_list_invariants(const List *list)
{
	if (list == NIL)
		return;

	Assert(list->length > 0);
	Assert(list->head != NULL);
	Assert(list->tail != NULL);

	Assert(list->type == T_List ||
		   list->type == T_IntList ||
		   list->type == T_OidList);

	if (list->length == 1)
		Assert(list->head == list->tail);
	if (list->length == 2)
		Assert(list->head->next == list->tail);
	Assert(list->tail->next == NULL);
}
#else
#define check_list_invariants(l)
#endif							/* USE_ASSERT_CHECKING */

/*
 * Return a freshly allocated List. Since empty non-NIL lists are
 * invalid, new_list() also allocates the head cell of the new list:
 * the caller should be sure to fill in that cell's data.
 */
static List *
new_list(NodeTag type)
{
	List	   *new_list;
	ListCell   *new_head;

	new_head = (ListCell *) palloc(sizeof(*new_head));
	new_head->next = NULL;
	/* new_head->data is left undefined! */

	new_list = (List *) palloc(sizeof(*new_list));
	new_list->type = type;
	new_list->length = 1;
	new_list->head = new_head;
	new_list->tail = new_head;

	return new_list;
}

/*
 * Allocate a new cell and make it the head of the specified
 * list. Assumes the list it is passed is non-NIL.
 *
 * The data in the new head cell is undefined; the caller should be
 * sure to fill it in
 */
static void
new_head_cell(List *list)
{
	ListCell   *new_head;

	new_head = (ListCell *) palloc(sizeof(*new_head));
	new_head->next = list->head;

	list->head = new_head;
	list->length++;
}

/*
 * Allocate a new cell and make it the tail of the specified
 * list. Assumes the list it is passed is non-NIL.
 *
 * The data in the new tail cell is undefined; the caller should be
 * sure to fill it in
 */
static void
new_tail_cell(List *list)
{
	ListCell   *new_tail;

	new_tail = (ListCell *) palloc(sizeof(*new_tail));
	new_tail->next = NULL;

	list->tail->next = new_tail;
	list->tail = new_tail;
	list->length++;
}

/*
 * Append a pointer to the list. A pointer to the modified list is
 * returned. Note that this function may or may not destructively
 * modify the list; callers should always use this function's return
 * value, rather than continuing to use the pointer passed as the
 * first argument.
 */
List *
lappend(List *list, void *datum)
{
	Assert(IsPointerList(list));

	if (list == NIL)
		list = new_list(T_List);
	else
		new_tail_cell(list);

	lfirst(list->tail) = datum;
	check_list_invariants(list);
	return list;
}

/*
 * Append an integer to the specified list. See lappend()
 */
List *
lappend_int(List *list, int datum)
{
	Assert(IsIntegerList(list));

	if (list == NIL)
		list = new_list(T_IntList);
	else
		new_tail_cell(list);

	lfirst_int(list->tail) = datum;
	check_list_invariants(list);
	return list;
}

/*
 * Append an OID to the specified list. See lappend()
 */
List *
lappend_oid(List *list, Oid datum)
{
	Assert(IsOidList(list));

	if (list == NIL)
		list = new_list(T_OidList);
	else
		new_tail_cell(list);

	lfirst_oid(list->tail) = datum;
	check_list_invariants(list);
	return list;
}

/*
 * Add a new cell to the list, in the position after 'prev_cell'. The
 * data in the cell is left undefined, and must be filled in by the
 * caller. 'list' is assumed to be non-NIL, and 'prev_cell' is assumed
 * to be non-NULL and a member of 'list'.
 */
static ListCell *
add_new_cell(List *list, ListCell *prev_cell)
{
	ListCell   *new_cell;

	new_cell = (ListCell *) palloc(sizeof(*new_cell));
	/* new_cell->data is left undefined! */
	new_cell->next = prev_cell->next;
	prev_cell->next = new_cell;

	if (list->tail == prev_cell)
		list->tail = new_cell;

	list->length++;

	return new_cell;
}

/*
 * 向指定链表（必须为非 NIL）添加一个新节点；
 * 新节点将被插入到 'prev' 节点之后（'prev' 必须非 NULL 且属于该链表）。
 * 新节点的数据为 'datum'，并返回新创建的节点指针。
 */
ListCell *
lappend_cell(List *list, ListCell *prev, void *datum)
{
	ListCell   *new_cell;

	Assert(IsPointerList(list));

	new_cell = add_new_cell(list, prev);
	lfirst(new_cell) = datum;
	check_list_invariants(list);
	return new_cell;
}

ListCell *
lappend_cell_int(List *list, ListCell *prev, int datum)
{
	ListCell   *new_cell;

	Assert(IsIntegerList(list));

	new_cell = add_new_cell(list, prev);
	lfirst_int(new_cell) = datum;
	check_list_invariants(list);
	return new_cell;
}

ListCell *
lappend_cell_oid(List *list, ListCell *prev, Oid datum)
{
	ListCell   *new_cell;

	Assert(IsOidList(list));

	new_cell = add_new_cell(list, prev);
	lfirst_oid(new_cell) = datum;
	check_list_invariants(list);
	return new_cell;
}

/*
 * 在链表头部插入一个新元素。返回修改后的链表指针。
 * 注意：该函数可能会破坏性地修改原链表，调用者应始终使用返回值，
 * 而不是继续使用传入的第二个参数。
 *
 * 注意：在 Postgres 8.0 之前，原始链表未被修改，可以认为保留了其独立性。
 * 现在已经不是这样了。
 */
List *
lcons(void *datum, List *list)
{
	Assert(IsPointerList(list));

	if (list == NIL)
		list = new_list(T_List);
	else
		new_head_cell(list);

	lfirst(list->head) = datum;
	check_list_invariants(list);
	return list;
}

/*
 * Prepend an integer to the list. See lcons()
 */
List *
lcons_int(int datum, List *list)
{
	Assert(IsIntegerList(list));

	if (list == NIL)
		list = new_list(T_IntList);
	else
		new_head_cell(list);

	lfirst_int(list->head) = datum;
	check_list_invariants(list);
	return list;
}

/*
 * Prepend an OID to the list. See lcons()
 */
List *
lcons_oid(Oid datum, List *list)
{
	Assert(IsOidList(list));

	if (list == NIL)
		list = new_list(T_OidList);
	else
		new_head_cell(list);

	lfirst_oid(list->head) = datum;
	check_list_invariants(list);
	return list;
}

/*
 * Concatenate list2 to the end of list1, and return list1. list1 is
 * destructively changed. Callers should be sure to use the return
 * value as the new pointer to the concatenated list: the 'list1'
 * input pointer may or may not be the same as the returned pointer.
 *
 * The nodes in list2 are merely appended to the end of list1 in-place
 * (i.e. they aren't copied; the two lists will share some of the same
 * storage). Therefore, invoking list_free() on list2 will also
 * invalidate a portion of list1.
 */
List *
list_concat(List *list1, List *list2)
{
	if (list1 == NIL)
		return list2;
	if (list2 == NIL)
		return list1;
	if (list1 == list2)
		elog(ERROR, "cannot list_concat() a list to itself");

	Assert(list1->type == list2->type);

	list1->length += list2->length;
	list1->tail->next = list2->head;
	list1->tail = list2->tail;

	check_list_invariants(list1);
	return list1;
}

/*
 * Truncate 'list' to contain no more than 'new_size' elements. This
 * modifies the list in-place! Despite this, callers should use the
 * pointer returned by this function to refer to the newly truncated
 * list -- it may or may not be the same as the pointer that was
 * passed.
 *
 * Note that any cells removed by list_truncate() are NOT pfree'd.
 */
List *
list_truncate(List *list, int new_size)
{
	ListCell   *cell;
	int			n;

	if (new_size <= 0)
		return NIL;				/* truncate to zero length */

	/* If asked to effectively extend the list, do nothing */
	if (new_size >= list_length(list))
		return list;

	n = 1;
	foreach(cell, list)
	{
		if (n == new_size)
		{
			cell->next = NULL;
			list->tail = cell;
			list->length = new_size;
			check_list_invariants(list);
			return list;
		}
		n++;
	}

	/* keep the compiler quiet; never reached */
	Assert(false);
	return list;
}

/*
 * Locate the n'th cell (counting from 0) of the list.  It is an assertion
 * failure if there is no such cell.
 */
ListCell *
list_nth_cell(const List *list, int n)
{
	ListCell   *match;

	Assert(list != NIL);
	Assert(n >= 0);
	Assert(n < list->length);
	check_list_invariants(list);

	/* Does the caller actually mean to fetch the tail? */
	if (n == list->length - 1)
		return list->tail;

	for (match = list->head; n-- > 0; match = match->next)
		;

	return match;
}

/*
 * Return the data value contained in the n'th element of the
 * specified list. (List elements begin at 0.)
 */
void *
list_nth(const List *list, int n)
{
	Assert(IsPointerList(list));
	return lfirst(list_nth_cell(list, n));
}

/*
 * Return the integer value contained in the n'th element of the
 * specified list.
 */
int
list_nth_int(const List *list, int n)
{
	Assert(IsIntegerList(list));
	return lfirst_int(list_nth_cell(list, n));
}

/*
 * Return the OID value contained in the n'th element of the specified
 * list.
 */
Oid
list_nth_oid(const List *list, int n)
{
	Assert(IsOidList(list));
	return lfirst_oid(list_nth_cell(list, n));
}

/*
 * Return true iff 'datum' is a member of the list. Equality is
 * determined via equal(), so callers should ensure that they pass a
 * Node as 'datum'.
 */
bool
list_member(const List *list, const void *datum)
{
	const ListCell *cell;

	Assert(IsPointerList(list));
	check_list_invariants(list);

	foreach(cell, list)
	{
		if (equal(lfirst(cell), datum))
			return true;
	}

	return false;
}

/*
 * Return true iff 'datum' is a member of the list. Equality is
 * determined by using simple pointer comparison.
 */
bool
list_member_ptr(const List *list, const void *datum)
{
	const ListCell *cell;

	Assert(IsPointerList(list));
	check_list_invariants(list);

	foreach(cell, list)
	{
		if (lfirst(cell) == datum)
			return true;
	}

	return false;
}

/*
 * Return true iff the integer 'datum' is a member of the list.
 */
bool
list_member_int(const List *list, int datum)
{
	const ListCell *cell;

	Assert(IsIntegerList(list));
	check_list_invariants(list);

	foreach(cell, list)
	{
		if (lfirst_int(cell) == datum)
			return true;
	}

	return false;
}

/*
 * Return true iff the OID 'datum' is a member of the list.
 */
bool
list_member_oid(const List *list, Oid datum)
{
	const ListCell *cell;

	Assert(IsOidList(list));
	check_list_invariants(list);

	foreach(cell, list)
	{
		if (lfirst_oid(cell) == datum)
			return true;
	}

	return false;
}

/*
 * Delete 'cell' from 'list'; 'prev' is the previous element to 'cell'
 * in 'list', if any (i.e. prev == NULL iff list->head == cell)
 *
 * The cell is pfree'd, as is the List header if this was the last member.
 */
List *
list_delete_cell(List *list, ListCell *cell, ListCell *prev)
{
	check_list_invariants(list);
	Assert(prev != NULL ? lnext(prev) == cell : list_head(list) == cell);

	/*
	 * If we're about to delete the last node from the list, free the whole
	 * list instead and return NIL, which is the only valid representation of
	 * a zero-length list.
	 */
	if (list->length == 1)
	{
		list_free(list);
		return NIL;
	}

	/*
	 * Otherwise, adjust the necessary list links, deallocate the particular
	 * node we have just removed, and return the list we were given.
	 */
	list->length--;

	if (prev)
		prev->next = cell->next;
	else
		list->head = cell->next;

	if (list->tail == cell)
		list->tail = prev;

	pfree(cell);
	return list;
}

/*
 * Delete the first cell in list that matches datum, if any.
 * Equality is determined via equal().
 */
List *
list_delete(List *list, void *datum)
{
	ListCell   *cell;
	ListCell   *prev;

	Assert(IsPointerList(list));
	check_list_invariants(list);

	prev = NULL;
	foreach(cell, list)
	{
		if (equal(lfirst(cell), datum))
			return list_delete_cell(list, cell, prev);

		prev = cell;
	}

	/* Didn't find a match: return the list unmodified */
	return list;
}

/* As above, but use simple pointer equality */
List *
list_delete_ptr(List *list, void *datum)
{
	ListCell   *cell;
	ListCell   *prev;

	Assert(IsPointerList(list));
	check_list_invariants(list);

	prev = NULL;
	foreach(cell, list)
	{
		if (lfirst(cell) == datum)
			return list_delete_cell(list, cell, prev);

		prev = cell;
	}

	/* Didn't find a match: return the list unmodified */
	return list;
}

/* As above, but for integers */
List *
list_delete_int(List *list, int datum)
{
	ListCell   *cell;
	ListCell   *prev;

	Assert(IsIntegerList(list));
	check_list_invariants(list);

	prev = NULL;
	foreach(cell, list)
	{
		if (lfirst_int(cell) == datum)
			return list_delete_cell(list, cell, prev);

		prev = cell;
	}

	/* Didn't find a match: return the list unmodified */
	return list;
}

/* As above, but for OIDs */
List *
list_delete_oid(List *list, Oid datum)
{
	ListCell   *cell;
	ListCell   *prev;

	Assert(IsOidList(list));
	check_list_invariants(list);

	prev = NULL;
	foreach(cell, list)
	{
		if (lfirst_oid(cell) == datum)
			return list_delete_cell(list, cell, prev);

		prev = cell;
	}

	/* Didn't find a match: return the list unmodified */
	return list;
}

/*
 * Delete the first element of the list.
 *
 * This is useful to replace the Lisp-y code "list = lnext(list);" in cases
 * where the intent is to alter the list rather than just traverse it.
 * Beware that the removed cell is freed, whereas the lnext() coding leaves
 * the original list head intact if there's another pointer to it.
 */
List *
list_delete_first(List *list)
{
	check_list_invariants(list);

	if (list == NIL)
		return NIL;				/* would an error be better? */

	return list_delete_cell(list, list_head(list), NULL);
}

/*
 * Generate the union of two lists. This is calculated by copying
 * list1 via list_copy(), then adding to it all the members of list2
 * that aren't already in list1.
 *
 * Whether an element is already a member of the list is determined
 * via equal().
 *
 * The returned list is newly-allocated, although the content of the
 * cells is the same (i.e. any pointed-to objects are not copied).
 *
 * NB: this function will NOT remove any duplicates that are present
 * in list1 (so it only performs a "union" if list1 is known unique to
 * start with).  Also, if you are about to write "x = list_union(x, y)"
 * you probably want to use list_concat_unique() instead to avoid wasting
 * the list cells of the old x list.
 *
 * This function could probably be implemented a lot faster if it is a
 * performance bottleneck.
 */
List *
list_union(const List *list1, const List *list2)
{
	List	   *result;
	const ListCell *cell;

	Assert(IsPointerList(list1));
	Assert(IsPointerList(list2));

	result = list_copy(list1);
	foreach(cell, list2)
	{
		if (!list_member(result, lfirst(cell)))
			result = lappend(result, lfirst(cell));
	}

	check_list_invariants(result);
	return result;
}

/*
 * This variant of list_union() determines duplicates via simple
 * pointer comparison.
 */
List *
list_union_ptr(const List *list1, const List *list2)
{
	List	   *result;
	const ListCell *cell;

	Assert(IsPointerList(list1));
	Assert(IsPointerList(list2));

	result = list_copy(list1);
	foreach(cell, list2)
	{
		if (!list_member_ptr(result, lfirst(cell)))
			result = lappend(result, lfirst(cell));
	}

	check_list_invariants(result);
	return result;
}

/*
 * This variant of list_union() operates upon lists of integers.
 */
List *
list_union_int(const List *list1, const List *list2)
{
	List	   *result;
	const ListCell *cell;

	Assert(IsIntegerList(list1));
	Assert(IsIntegerList(list2));

	result = list_copy(list1);
	foreach(cell, list2)
	{
		if (!list_member_int(result, lfirst_int(cell)))
			result = lappend_int(result, lfirst_int(cell));
	}

	check_list_invariants(result);
	return result;
}

/*
 * This variant of list_union() operates upon lists of OIDs.
 */
List *
list_union_oid(const List *list1, const List *list2)
{
	List	   *result;
	const ListCell *cell;

	Assert(IsOidList(list1));
	Assert(IsOidList(list2));

	result = list_copy(list1);
	foreach(cell, list2)
	{
		if (!list_member_oid(result, lfirst_oid(cell)))
			result = lappend_oid(result, lfirst_oid(cell));
	}

	check_list_invariants(result);
	return result;
}

/*
 * Return a list that contains all the cells that are in both list1 and
 * list2.  The returned list is freshly allocated via palloc(), but the
 * cells themselves point to the same objects as the cells of the
 * input lists.
 *
 * Duplicate entries in list1 will not be suppressed, so it's only a true
 * "intersection" if list1 is known unique beforehand.
 *
 * This variant works on lists of pointers, and determines list
 * membership via equal().  Note that the list1 member will be pointed
 * to in the result.
 */
List *
list_intersection(const List *list1, const List *list2)
{
	List	   *result;
	const ListCell *cell;

	if (list1 == NIL || list2 == NIL)
		return NIL;

	Assert(IsPointerList(list1));
	Assert(IsPointerList(list2));

	result = NIL;
	foreach(cell, list1)
	{
		if (list_member(list2, lfirst(cell)))
			result = lappend(result, lfirst(cell));
	}

	check_list_invariants(result);
	return result;
}

/*
 * As list_intersection but operates on lists of integers.
 */
List *
list_intersection_int(const List *list1, const List *list2)
{
	List	   *result;
	const ListCell *cell;

	if (list1 == NIL || list2 == NIL)
		return NIL;

	Assert(IsIntegerList(list1));
	Assert(IsIntegerList(list2));

	result = NIL;
	foreach(cell, list1)
	{
		if (list_member_int(list2, lfirst_int(cell)))
			result = lappend_int(result, lfirst_int(cell));
	}

	check_list_invariants(result);
	return result;
}

/*
 * Return a list that contains all the cells in list1 that are not in
 * list2. The returned list is freshly allocated via palloc(), but the
 * cells themselves point to the same objects as the cells of the
 * input lists.
 *
 * This variant works on lists of pointers, and determines list
 * membership via equal()
 */
List *
list_difference(const List *list1, const List *list2)
{
	const ListCell *cell;
	List	   *result = NIL;

	Assert(IsPointerList(list1));
	Assert(IsPointerList(list2));

	if (list2 == NIL)
		return list_copy(list1);

	foreach(cell, list1)
	{
		if (!list_member(list2, lfirst(cell)))
			result = lappend(result, lfirst(cell));
	}

	check_list_invariants(result);
	return result;
}

/*
 * This variant of list_difference() determines list membership via
 * simple pointer equality.
 */
List *
list_difference_ptr(const List *list1, const List *list2)
{
	const ListCell *cell;
	List	   *result = NIL;

	Assert(IsPointerList(list1));
	Assert(IsPointerList(list2));

	if (list2 == NIL)
		return list_copy(list1);

	foreach(cell, list1)
	{
		if (!list_member_ptr(list2, lfirst(cell)))
			result = lappend(result, lfirst(cell));
	}

	check_list_invariants(result);
	return result;
}

/*
 * This variant of list_difference() operates upon lists of integers.
 */
List *
list_difference_int(const List *list1, const List *list2)
{
	const ListCell *cell;
	List	   *result = NIL;

	Assert(IsIntegerList(list1));
	Assert(IsIntegerList(list2));

	if (list2 == NIL)
		return list_copy(list1);

	foreach(cell, list1)
	{
		if (!list_member_int(list2, lfirst_int(cell)))
			result = lappend_int(result, lfirst_int(cell));
	}

	check_list_invariants(result);
	return result;
}

/*
 * This variant of list_difference() operates upon lists of OIDs.
 */
List *
list_difference_oid(const List *list1, const List *list2)
{
	const ListCell *cell;
	List	   *result = NIL;

	Assert(IsOidList(list1));
	Assert(IsOidList(list2));

	if (list2 == NIL)
		return list_copy(list1);

	foreach(cell, list1)
	{
		if (!list_member_oid(list2, lfirst_oid(cell)))
			result = lappend_oid(result, lfirst_oid(cell));
	}

	check_list_invariants(result);
	return result;
}

/*
 * Append datum to list, but only if it isn't already in the list.
 *
 * Whether an element is already a member of the list is determined
 * via equal().
 */
List *
list_append_unique(List *list, void *datum)
{
	if (list_member(list, datum))
		return list;
	else
		return lappend(list, datum);
}

/*
 * This variant of list_append_unique() determines list membership via
 * simple pointer equality.
 */
List *
list_append_unique_ptr(List *list, void *datum)
{
	if (list_member_ptr(list, datum))
		return list;
	else
		return lappend(list, datum);
}

/*
 * This variant of list_append_unique() operates upon lists of integers.
 */
List *
list_append_unique_int(List *list, int datum)
{
	if (list_member_int(list, datum))
		return list;
	else
		return lappend_int(list, datum);
}

/*
 * This variant of list_append_unique() operates upon lists of OIDs.
 */
List *
list_append_unique_oid(List *list, Oid datum)
{
	if (list_member_oid(list, datum))
		return list;
	else
		return lappend_oid(list, datum);
}

/*
 * Append to list1 each member of list2 that isn't already in list1.
 *
 * Whether an element is already a member of the list is determined
 * via equal().
 *
 * This is almost the same functionality as list_union(), but list1 is
 * modified in-place rather than being copied. However, callers of this
 * function may have strict ordering expectations -- i.e. that the relative
 * order of those list2 elements that are not duplicates is preserved. Note
 * also that list2's cells are not inserted in list1, so the analogy to
 * list_concat() isn't perfect.
 */
List *
list_concat_unique(List *list1, List *list2)
{
	ListCell   *cell;

	Assert(IsPointerList(list1));
	Assert(IsPointerList(list2));

	foreach(cell, list2)
	{
		if (!list_member(list1, lfirst(cell)))
			list1 = lappend(list1, lfirst(cell));
	}

	check_list_invariants(list1);
	return list1;
}

/*
 * This variant of list_concat_unique() determines list membership via
 * simple pointer equality.
 */
List *
list_concat_unique_ptr(List *list1, List *list2)
{
	ListCell   *cell;

	Assert(IsPointerList(list1));
	Assert(IsPointerList(list2));

	foreach(cell, list2)
	{
		if (!list_member_ptr(list1, lfirst(cell)))
			list1 = lappend(list1, lfirst(cell));
	}

	check_list_invariants(list1);
	return list1;
}

/*
 * This variant of list_concat_unique() operates upon lists of integers.
 */
List *
list_concat_unique_int(List *list1, List *list2)
{
	ListCell   *cell;

	Assert(IsIntegerList(list1));
	Assert(IsIntegerList(list2));

	foreach(cell, list2)
	{
		if (!list_member_int(list1, lfirst_int(cell)))
			list1 = lappend_int(list1, lfirst_int(cell));
	}

	check_list_invariants(list1);
	return list1;
}

/*
 * This variant of list_concat_unique() operates upon lists of OIDs.
 */
List *
list_concat_unique_oid(List *list1, List *list2)
{
	ListCell   *cell;

	Assert(IsOidList(list1));
	Assert(IsOidList(list2));

	foreach(cell, list2)
	{
		if (!list_member_oid(list1, lfirst_oid(cell)))
			list1 = lappend_oid(list1, lfirst_oid(cell));
	}

	check_list_invariants(list1);
	return list1;
}

/*
 * Free all storage in a list, and optionally the pointed-to elements
 */
static void
list_free_private(List *list, bool deep)
{
	ListCell   *cell;

	check_list_invariants(list);

	cell = list_head(list);
	while (cell != NULL)
	{
		ListCell   *tmp = cell;

		cell = lnext(cell);
		if (deep)
			pfree(lfirst(tmp));
		pfree(tmp);
	}

	if (list)
		pfree(list);
}

/*
 * Free all the cells of the list, as well as the list itself. Any
 * objects that are pointed-to by the cells of the list are NOT
 * free'd.
 *
 * On return, the argument to this function has been freed, so the
 * caller would be wise to set it to NIL for safety's sake.
 */
void
list_free(List *list)
{
	list_free_private(list, false);
}

/*
 * Free all the cells of the list, the list itself, and all the
 * objects pointed-to by the cells of the list (each element in the
 * list must contain a pointer to a palloc()'d region of memory!)
 *
 * On return, the argument to this function has been freed, so the
 * caller would be wise to set it to NIL for safety's sake.
 */
void
list_free_deep(List *list)
{
	/*
	 * A "deep" free operation only makes sense on a list of pointers.
	 */
	Assert(IsPointerList(list));
	list_free_private(list, true);
}

/*
 * list_copy - 创建指定列表的浅拷贝
 * 
 * @param oldlist: 要复制的源列表指针
 * @return: 新创建的列表副本；如果源列表为空（NIL），则返回NIL
 * 
 * 注意：这是浅拷贝操作，只复制列表结构和元素引用，不复制元素指向的实际对象
 */
List *
list_copy(const List *oldlist)
{
	List	   *newlist;      /* 新创建的列表指针 */
	ListCell   *newlist_prev; /* 新列表中当前节点的前一个节点，用于链表构建 */
	ListCell   *oldlist_cur;  /* 源列表中当前遍历到的节点 */

	/* 边界检查：如果源列表为空，直接返回空列表 */
	if (oldlist == NIL)
		return NIL;

	/* 创建一个新的列表结构，使用与源列表相同的类型（T_List、T_IntList或T_OidList） */
	newlist = new_list(oldlist->type);
	/* 设置新列表的长度与源列表相同 */
	newlist->length = oldlist->length;

	/*
	 * 复制第一个单元格的数据；new_list()函数已经为新列表分配了头单元格
	 * 只需要将源列表头单元格的数据复制到新列表头单元格即可
	 */
	newlist->head->data = oldlist->head->data;

	/* 初始化遍历指针：
	 * - newlist_prev：指向新列表的头单元格（已复制数据）
	 * - oldlist_cur：指向源列表的第二个单元格（第一个单元格已复制）
	 */
	newlist_prev = newlist->head;
	oldlist_cur = oldlist->head->next;
	
	/* 遍历源列表中剩余的所有节点并复制 */
	while (oldlist_cur)
	{
		ListCell   *newlist_cur; /* 新列表中要创建的当前节点 */

		/* 为新节点分配内存空间 */
		newlist_cur = (ListCell *) palloc(sizeof(*newlist_cur));
		/* 复制源节点的数据（浅拷贝：只复制指针或值，不复制指向的对象） */
		newlist_cur->data = oldlist_cur->data;
		/* 将新节点链接到新列表中，作为前一个节点的下一个节点 */
		newlist_prev->next = newlist_cur;

		/* 更新遍历指针，准备处理下一个节点 */
		newlist_prev = newlist_cur;
		oldlist_cur = oldlist_cur->next;
	}

	/* 设置新列表的结束标记：最后一个节点的next指针设为NULL */
	newlist_prev->next = NULL;
	/* 更新新列表的尾指针，指向最后一个节点 */
	newlist->tail = newlist_prev;

	/* 验证新列表的结构完整性，确保长度、头指针、尾指针等关系正确 */
	check_list_invariants(newlist);
	/* 返回新创建的列表副本 */
	return newlist;
}


/*
 * list_copy_tail - 创建列表的浅拷贝，不包含前N个元素
 * 
 * @param oldlist: 要复制的源列表
 * @param nskip: 要跳过的元素数量
 * @return: 新创建的列表，包含源列表中跳过前nskip个元素后的所有元素；
 *          如果源列表为空或nskip大于等于列表长度，则返回NIL
 */
List *
list_copy_tail(const List *oldlist, int nskip)
{
	List	   *newlist;      /* 新创建的列表指针 */
	ListCell   *newlist_prev; /* 新列表中当前节点的前一个节点 */
	ListCell   *oldlist_cur;  /* 源列表中当前遍历到的节点 */

	/* 确保跳过的元素数量不为负数 */
	if (nskip < 0)
		nskip = 0;              /* 这里可以考虑使用elog报错，但当前选择了容错处理 */

	/* 边界检查：如果源列表为空或跳过的元素数超过列表长度，返回空列表 */
	if (oldlist == NIL || nskip >= oldlist->length)
		return NIL;

	/* 创建一个新的列表结构，使用与源列表相同的类型 */
	newlist = new_list(oldlist->type);
	/* 设置新列表的长度：源列表长度减去跳过的元素数量 */
	newlist->length = oldlist->length - nskip;

	/*
	 * 定位到源列表中要开始复制的位置
	 */
	oldlist_cur = oldlist->head;
	while (nskip-- > 0)
		oldlist_cur = oldlist_cur->next;

	/*
	 * 复制第一个剩余元素的数据；new_list()已经分配了新列表的头节点
	 */
	newlist->head->data = oldlist_cur->data;

	/* 初始化遍历指针：新列表从头部开始，源列表指向下一个要复制的节点 */
	newlist_prev = newlist->head;
	oldlist_cur = oldlist_cur->next;
	
	/* 遍历源列表中剩余的所有节点并复制 */
	while (oldlist_cur)
	{
		ListCell   *newlist_cur; /* 新列表中要创建的当前节点 */

		/* 为新节点分配内存 */
		newlist_cur = (ListCell *) palloc(sizeof(*newlist_cur));
		/* 复制源节点的数据（浅拷贝，只复制指针/值，不复制指向的对象） */
		newlist_cur->data = oldlist_cur->data;
		/* 将新节点链接到新列表中 */
		newlist_prev->next = newlist_cur;

		/* 更新遍历指针 */
		newlist_prev = newlist_cur;
		oldlist_cur = oldlist_cur->next;
	}

	/* 设置新列表的结束标记 */
	newlist_prev->next = NULL;
	/* 更新新列表的尾指针 */
	newlist->tail = newlist_prev;

	/* 验证新列表的结构完整性 */
	check_list_invariants(newlist);
	/* 返回创建的新列表 */
	return newlist;
}


/*
 * Sort a list as though by qsort.
 *
 * A new list is built and returned.  Like list_copy, this doesn't make
 * fresh copies of any pointed-to data.
 *
 * The comparator function receives arguments of type ListCell **.
 */
List *
list_qsort(const List *list, list_qsort_comparator cmp)
{
	int			len = list_length(list);
	ListCell  **list_arr;
	List	   *newlist;
	ListCell   *newlist_prev;
	ListCell   *cell;
	int			i;

	/* Empty list is easy */
	if (len == 0)
		return NIL;

	/* Flatten list cells into an array, so we can use qsort */
	list_arr = (ListCell **) palloc(sizeof(ListCell *) * len);
	i = 0;
	foreach(cell, list)
		list_arr[i++] = cell;

	qsort(list_arr, len, sizeof(ListCell *), cmp);

	/* Construct new list (this code is much like list_copy) */
	newlist = new_list(list->type);
	newlist->length = len;

	/*
	 * Copy over the data in the first cell; new_list() has already allocated
	 * the head cell itself
	 */
	newlist->head->data = list_arr[0]->data;

	newlist_prev = newlist->head;
	for (i = 1; i < len; i++)
	{
		ListCell   *newlist_cur;

		newlist_cur = (ListCell *) palloc(sizeof(*newlist_cur));
		newlist_cur->data = list_arr[i]->data;
		newlist_prev->next = newlist_cur;

		newlist_prev = newlist_cur;
	}

	newlist_prev->next = NULL;
	newlist->tail = newlist_prev;

	/* Might as well free the workspace array */
	pfree(list_arr);

	check_list_invariants(newlist);
	return newlist;
}

/*
 * Temporary compatibility functions
 *
 * In order to avoid warnings for these function definitions, we need
 * to include a prototype here as well as in pg_list.h. That's because
 * we don't enable list API compatibility in list.c, so we
 * don't see the prototypes for these functions.
 */

/*
 * Given a list, return its length. This is merely defined for the
 * sake of backward compatibility: we can't afford to define a macro
 * called "length", so it must be a function. New code should use the
 * list_length() macro in order to avoid the overhead of a function
 * call.
 */
int			length(const List *list);

int
length(const List *list)
{
	return list_length(list);
}
