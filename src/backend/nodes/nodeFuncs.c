/*-------------------------------------------------------------------------
 *
 * nodeFuncs.c
 *		Various general-purpose manipulations of Node trees
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/nodes/nodeFuncs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_collation.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/execnodes.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pathnodes.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"


static bool expression_returns_set_walker(Node *node, void *context);
static int	leftmostLoc(int loc1, int loc2);
static bool fix_opfuncids_walker(Node *node, void *context);
static bool planstate_walk_subplans(List *plans, bool (*walker) (),
									void *context);
static bool planstate_walk_members(PlanState **planstates, int nplans,
								   bool (*walker) (), void *context);


/*
 *	exprType -
 *	  returns the Oid of the type of the expression's result.
 */
Oid
exprType(const Node *expr)
{
	Oid			type;

	if (!expr)
		return InvalidOid;

	switch (nodeTag(expr))
	{
		case T_Var:
			type = ((const Var *) expr)->vartype;
			break;
		case T_Const:
			type = ((const Const *) expr)->consttype;
			break;
		case T_Param:
			type = ((const Param *) expr)->paramtype;
			break;
		case T_Aggref:
			type = ((const Aggref *) expr)->aggtype;
			break;
		case T_GroupingFunc:
			type = INT4OID;
			break;
		case T_WindowFunc:
			type = ((const WindowFunc *) expr)->wintype;
			break;
		case T_SubscriptingRef:
			{
				const SubscriptingRef *sbsref = (const SubscriptingRef *) expr;

				/* slice and/or store operations yield the container type */
				if (sbsref->reflowerindexpr || sbsref->refassgnexpr)
					type = sbsref->refcontainertype;
				else
					type = sbsref->refelemtype;
			}
			break;
		case T_FuncExpr:
			type = ((const FuncExpr *) expr)->funcresulttype;
			break;
		case T_NamedArgExpr:
			type = exprType((Node *) ((const NamedArgExpr *) expr)->arg);
			break;
		case T_OpExpr:
			type = ((const OpExpr *) expr)->opresulttype;
			break;
		case T_DistinctExpr:
			type = ((const DistinctExpr *) expr)->opresulttype;
			break;
		case T_NullIfExpr:
			type = ((const NullIfExpr *) expr)->opresulttype;
			break;
		case T_ScalarArrayOpExpr:
			type = BOOLOID;
			break;
		case T_BoolExpr:
			type = BOOLOID;
			break;
		case T_SubLink:
			{
				const SubLink *sublink = (const SubLink *) expr;

				if (sublink->subLinkType == EXPR_SUBLINK ||
					sublink->subLinkType == ARRAY_SUBLINK)
				{
					/* get the type of the subselect's first target column */
					Query	   *qtree = (Query *) sublink->subselect;
					TargetEntry *tent;

					if (!qtree || !IsA(qtree, Query))
						elog(ERROR, "cannot get type for untransformed sublink");
					tent = linitial_node(TargetEntry, qtree->targetList);
					Assert(!tent->resjunk);
					type = exprType((Node *) tent->expr);
					if (sublink->subLinkType == ARRAY_SUBLINK)
					{
						type = get_promoted_array_type(type);
						if (!OidIsValid(type))
							ereport(ERROR,
									(errcode(ERRCODE_UNDEFINED_OBJECT),
									 errmsg("could not find array type for data type %s",
											format_type_be(exprType((Node *) tent->expr)))));
					}
				}
				else if (sublink->subLinkType == MULTIEXPR_SUBLINK)
				{
					/* MULTIEXPR is always considered to return RECORD */
					type = RECORDOID;
				}
				else
				{
					/* for all other sublink types, result is boolean */
					type = BOOLOID;
				}
			}
			break;
		case T_SubPlan:
			{
				const SubPlan *subplan = (const SubPlan *) expr;

				if (subplan->subLinkType == EXPR_SUBLINK ||
					subplan->subLinkType == ARRAY_SUBLINK)
				{
					/* get the type of the subselect's first target column */
					type = subplan->firstColType;
					if (subplan->subLinkType == ARRAY_SUBLINK)
					{
						type = get_promoted_array_type(type);
						if (!OidIsValid(type))
							ereport(ERROR,
									(errcode(ERRCODE_UNDEFINED_OBJECT),
									 errmsg("could not find array type for data type %s",
											format_type_be(subplan->firstColType))));
					}
				}
				else if (subplan->subLinkType == MULTIEXPR_SUBLINK)
				{
					/* MULTIEXPR is always considered to return RECORD */
					type = RECORDOID;
				}
				else
				{
					/* for all other subplan types, result is boolean */
					type = BOOLOID;
				}
			}
			break;
		case T_AlternativeSubPlan:
			{
				const AlternativeSubPlan *asplan = (const AlternativeSubPlan *) expr;

				/* subplans should all return the same thing */
				type = exprType((Node *) linitial(asplan->subplans));
			}
			break;
		case T_FieldSelect:
			type = ((const FieldSelect *) expr)->resulttype;
			break;
		case T_FieldStore:
			type = ((const FieldStore *) expr)->resulttype;
			break;
		case T_RelabelType:
			type = ((const RelabelType *) expr)->resulttype;
			break;
		case T_CoerceViaIO:
			type = ((const CoerceViaIO *) expr)->resulttype;
			break;
		case T_ArrayCoerceExpr:
			type = ((const ArrayCoerceExpr *) expr)->resulttype;
			break;
		case T_ConvertRowtypeExpr:
			type = ((const ConvertRowtypeExpr *) expr)->resulttype;
			break;
		case T_CollateExpr:
			type = exprType((Node *) ((const CollateExpr *) expr)->arg);
			break;
		case T_CaseExpr:
			type = ((const CaseExpr *) expr)->casetype;
			break;
		case T_CaseTestExpr:
			type = ((const CaseTestExpr *) expr)->typeId;
			break;
		case T_ArrayExpr:
			type = ((const ArrayExpr *) expr)->array_typeid;
			break;
		case T_RowExpr:
			type = ((const RowExpr *) expr)->row_typeid;
			break;
		case T_RowCompareExpr:
			type = BOOLOID;
			break;
		case T_CoalesceExpr:
			type = ((const CoalesceExpr *) expr)->coalescetype;
			break;
		case T_MinMaxExpr:
			type = ((const MinMaxExpr *) expr)->minmaxtype;
			break;
		case T_SQLValueFunction:
			type = ((const SQLValueFunction *) expr)->type;
			break;
		case T_XmlExpr:
			if (((const XmlExpr *) expr)->op == IS_DOCUMENT)
				type = BOOLOID;
			else if (((const XmlExpr *) expr)->op == IS_XMLSERIALIZE)
				type = TEXTOID;
			else
				type = XMLOID;
			break;
		case T_NullTest:
			type = BOOLOID;
			break;
		case T_BooleanTest:
			type = BOOLOID;
			break;
		case T_CoerceToDomain:
			type = ((const CoerceToDomain *) expr)->resulttype;
			break;
		case T_CoerceToDomainValue:
			type = ((const CoerceToDomainValue *) expr)->typeId;
			break;
		case T_SetToDefault:
			type = ((const SetToDefault *) expr)->typeId;
			break;
		case T_CurrentOfExpr:
			type = BOOLOID;
			break;
		case T_NextValueExpr:
			type = ((const NextValueExpr *) expr)->typeId;
			break;
		case T_InferenceElem:
			{
				const InferenceElem *n = (const InferenceElem *) expr;

				type = exprType((Node *) n->expr);
			}
			break;
		case T_PlaceHolderVar:
			type = exprType((Node *) ((const PlaceHolderVar *) expr)->phexpr);
			break;
		default:
			elog(ERROR, "unrecognized node type: %d", (int) nodeTag(expr));
			type = InvalidOid;	/* keep compiler quiet */
			break;
	}
	return type;
}

/*
 *	exprTypmod -
 *	  returns the type-specific modifier of the expression's result type,
 *	  if it can be determined.  In many cases, it can't and we return -1.
 */
int32
exprTypmod(const Node *expr)
{
	if (!expr)
		return -1;

	switch (nodeTag(expr))
	{
		case T_Var:
			return ((const Var *) expr)->vartypmod;
		case T_Const:
			return ((const Const *) expr)->consttypmod;
		case T_Param:
			return ((const Param *) expr)->paramtypmod;
		case T_SubscriptingRef:
			/* typmod is the same for container or element */
			return ((const SubscriptingRef *) expr)->reftypmod;
		case T_FuncExpr:
			{
				int32		coercedTypmod;

				/* Be smart about length-coercion functions... */
				if (exprIsLengthCoercion(expr, &coercedTypmod))
					return coercedTypmod;
			}
			break;
		case T_NamedArgExpr:
			return exprTypmod((Node *) ((const NamedArgExpr *) expr)->arg);
		case T_NullIfExpr:
			{
				/*
				 * Result is either first argument or NULL, so we can report
				 * first argument's typmod if known.
				 */
				const NullIfExpr *nexpr = (const NullIfExpr *) expr;

				return exprTypmod((Node *) linitial(nexpr->args));
			}
			break;
		case T_SubLink:
			{
				const SubLink *sublink = (const SubLink *) expr;

				if (sublink->subLinkType == EXPR_SUBLINK ||
					sublink->subLinkType == ARRAY_SUBLINK)
				{
					/* get the typmod of the subselect's first target column */
					Query	   *qtree = (Query *) sublink->subselect;
					TargetEntry *tent;

					if (!qtree || !IsA(qtree, Query))
						elog(ERROR, "cannot get type for untransformed sublink");
					tent = linitial_node(TargetEntry, qtree->targetList);
					Assert(!tent->resjunk);
					return exprTypmod((Node *) tent->expr);
					/* note we don't need to care if it's an array */
				}
				/* otherwise, result is RECORD or BOOLEAN, typmod is -1 */
			}
			break;
		case T_SubPlan:
			{
				const SubPlan *subplan = (const SubPlan *) expr;

				if (subplan->subLinkType == EXPR_SUBLINK ||
					subplan->subLinkType == ARRAY_SUBLINK)
				{
					/* get the typmod of the subselect's first target column */
					/* note we don't need to care if it's an array */
					return subplan->firstColTypmod;
				}
				/* otherwise, result is RECORD or BOOLEAN, typmod is -1 */
			}
			break;
		case T_AlternativeSubPlan:
			{
				const AlternativeSubPlan *asplan = (const AlternativeSubPlan *) expr;

				/* subplans should all return the same thing */
				return exprTypmod((Node *) linitial(asplan->subplans));
			}
			break;
		case T_FieldSelect:
			return ((const FieldSelect *) expr)->resulttypmod;
		case T_RelabelType:
			return ((const RelabelType *) expr)->resulttypmod;
		case T_ArrayCoerceExpr:
			return ((const ArrayCoerceExpr *) expr)->resulttypmod;
		case T_CollateExpr:
			return exprTypmod((Node *) ((const CollateExpr *) expr)->arg);
		case T_CaseExpr:
			{
				/*
				 * If all the alternatives agree on type/typmod, return that
				 * typmod, else use -1
				 */
				const CaseExpr *cexpr = (const CaseExpr *) expr;
				Oid			casetype = cexpr->casetype;
				int32		typmod;
				ListCell   *arg;

				if (!cexpr->defresult)
					return -1;
				if (exprType((Node *) cexpr->defresult) != casetype)
					return -1;
				typmod = exprTypmod((Node *) cexpr->defresult);
				if (typmod < 0)
					return -1;	/* no point in trying harder */
				foreach(arg, cexpr->args)
				{
					CaseWhen   *w = lfirst_node(CaseWhen, arg);

					if (exprType((Node *) w->result) != casetype)
						return -1;
					if (exprTypmod((Node *) w->result) != typmod)
						return -1;
				}
				return typmod;
			}
			break;
		case T_CaseTestExpr:
			return ((const CaseTestExpr *) expr)->typeMod;
		case T_ArrayExpr:
			{
				/*
				 * If all the elements agree on type/typmod, return that
				 * typmod, else use -1
				 */
				const ArrayExpr *arrayexpr = (const ArrayExpr *) expr;
				Oid			commontype;
				int32		typmod;
				ListCell   *elem;

				if (arrayexpr->elements == NIL)
					return -1;
				typmod = exprTypmod((Node *) linitial(arrayexpr->elements));
				if (typmod < 0)
					return -1;	/* no point in trying harder */
				if (arrayexpr->multidims)
					commontype = arrayexpr->array_typeid;
				else
					commontype = arrayexpr->element_typeid;
				foreach(elem, arrayexpr->elements)
				{
					Node	   *e = (Node *) lfirst(elem);

					if (exprType(e) != commontype)
						return -1;
					if (exprTypmod(e) != typmod)
						return -1;
				}
				return typmod;
			}
			break;
		case T_CoalesceExpr:
			{
				/*
				 * If all the alternatives agree on type/typmod, return that
				 * typmod, else use -1
				 */
				const CoalesceExpr *cexpr = (const CoalesceExpr *) expr;
				Oid			coalescetype = cexpr->coalescetype;
				int32		typmod;
				ListCell   *arg;

				if (exprType((Node *) linitial(cexpr->args)) != coalescetype)
					return -1;
				typmod = exprTypmod((Node *) linitial(cexpr->args));
				if (typmod < 0)
					return -1;	/* no point in trying harder */
				for_each_cell(arg, lnext(list_head(cexpr->args)))
				{
					Node	   *e = (Node *) lfirst(arg);

					if (exprType(e) != coalescetype)
						return -1;
					if (exprTypmod(e) != typmod)
						return -1;
				}
				return typmod;
			}
			break;
		case T_MinMaxExpr:
			{
				/*
				 * If all the alternatives agree on type/typmod, return that
				 * typmod, else use -1
				 */
				const MinMaxExpr *mexpr = (const MinMaxExpr *) expr;
				Oid			minmaxtype = mexpr->minmaxtype;
				int32		typmod;
				ListCell   *arg;

				if (exprType((Node *) linitial(mexpr->args)) != minmaxtype)
					return -1;
				typmod = exprTypmod((Node *) linitial(mexpr->args));
				if (typmod < 0)
					return -1;	/* no point in trying harder */
				for_each_cell(arg, lnext(list_head(mexpr->args)))
				{
					Node	   *e = (Node *) lfirst(arg);

					if (exprType(e) != minmaxtype)
						return -1;
					if (exprTypmod(e) != typmod)
						return -1;
				}
				return typmod;
			}
			break;
		case T_SQLValueFunction:
			return ((const SQLValueFunction *) expr)->typmod;
		case T_CoerceToDomain:
			return ((const CoerceToDomain *) expr)->resulttypmod;
		case T_CoerceToDomainValue:
			return ((const CoerceToDomainValue *) expr)->typeMod;
		case T_SetToDefault:
			return ((const SetToDefault *) expr)->typeMod;
		case T_PlaceHolderVar:
			return exprTypmod((Node *) ((const PlaceHolderVar *) expr)->phexpr);
		default:
			break;
	}
	return -1;
}

/*
 * exprIsLengthCoercion
 *		Detect whether an expression tree is an application of a datatype's
 *		typmod-coercion function.  Optionally extract the result's typmod.
 *
 * If coercedTypmod is not NULL, the typmod is stored there if the expression
 * is a length-coercion function, else -1 is stored there.
 *
 * Note that a combined type-and-length coercion will be treated as a
 * length coercion by this routine.
 */
bool
exprIsLengthCoercion(const Node *expr, int32 *coercedTypmod)
{
	if (coercedTypmod != NULL)
		*coercedTypmod = -1;	/* default result on failure */

	/*
	 * Scalar-type length coercions are FuncExprs, array-type length coercions
	 * are ArrayCoerceExprs
	 */
	if (expr && IsA(expr, FuncExpr))
	{
		const FuncExpr *func = (const FuncExpr *) expr;
		int			nargs;
		Const	   *second_arg;

		/*
		 * If it didn't come from a coercion context, reject.
		 */
		if (func->funcformat != COERCE_EXPLICIT_CAST &&
			func->funcformat != COERCE_IMPLICIT_CAST)
			return false;

		/*
		 * If it's not a two-argument or three-argument function with the
		 * second argument being an int4 constant, it can't have been created
		 * from a length coercion (it must be a type coercion, instead).
		 */
		nargs = list_length(func->args);
		if (nargs < 2 || nargs > 3)
			return false;

		second_arg = (Const *) lsecond(func->args);
		if (!IsA(second_arg, Const) ||
			second_arg->consttype != INT4OID ||
			second_arg->constisnull)
			return false;

		/*
		 * OK, it is indeed a length-coercion function.
		 */
		if (coercedTypmod != NULL)
			*coercedTypmod = DatumGetInt32(second_arg->constvalue);

		return true;
	}

	if (expr && IsA(expr, ArrayCoerceExpr))
	{
		const ArrayCoerceExpr *acoerce = (const ArrayCoerceExpr *) expr;

		/* It's not a length coercion unless there's a nondefault typmod */
		if (acoerce->resulttypmod < 0)
			return false;

		/*
		 * OK, it is indeed a length-coercion expression.
		 */
		if (coercedTypmod != NULL)
			*coercedTypmod = acoerce->resulttypmod;

		return true;
	}

	return false;
}

/*
 * applyRelabelType
 *		Add a RelabelType node if needed to make the expression expose
 *		the specified type, typmod, and collation.
 *
 * This is primarily intended to be used during planning.  Therefore, it must
 * maintain the post-eval_const_expressions invariants that there are not
 * adjacent RelabelTypes, and that the tree is fully const-folded (hence,
 * we mustn't return a RelabelType atop a Const).  If we do find a Const,
 * we'll modify it in-place if "overwrite_ok" is true; that should only be
 * passed as true if caller knows the Const is newly generated.
 */
Node *
applyRelabelType(Node *arg, Oid rtype, int32 rtypmod, Oid rcollid,
				 CoercionForm rformat, int rlocation, bool overwrite_ok)
{
	/*
	 * If we find stacked RelabelTypes (eg, from foo::int::oid) we can discard
	 * all but the top one, and must do so to ensure that semantically
	 * equivalent expressions are equal().
	 */
	while (arg && IsA(arg, RelabelType))
		arg = (Node *) ((RelabelType *) arg)->arg;

	if (arg && IsA(arg, Const))
	{
		/* Modify the Const directly to preserve const-flatness. */
		Const	   *con = (Const *) arg;

		if (!overwrite_ok)
			con = copyObject(con);
		con->consttype = rtype;
		con->consttypmod = rtypmod;
		con->constcollid = rcollid;
		/* We keep the Const's original location. */
		return (Node *) con;
	}
	else if (exprType(arg) == rtype &&
			 exprTypmod(arg) == rtypmod &&
			 exprCollation(arg) == rcollid)
	{
		/* Sometimes we find a nest of relabels that net out to nothing. */
		return arg;
	}
	else
	{
		/* Nope, gotta have a RelabelType. */
		RelabelType *newrelabel = makeNode(RelabelType);

		newrelabel->arg = (Expr *) arg;
		newrelabel->resulttype = rtype;
		newrelabel->resulttypmod = rtypmod;
		newrelabel->resultcollid = rcollid;
		newrelabel->relabelformat = rformat;
		newrelabel->location = rlocation;
		return (Node *) newrelabel;
	}
}

/*
 * relabel_to_typmod
 *		Add a RelabelType node that changes just the typmod of the expression.
 *
 * Convenience function for a common usage of applyRelabelType.
 */
Node *
relabel_to_typmod(Node *expr, int32 typmod)
{
	return applyRelabelType(expr, exprType(expr), typmod, exprCollation(expr),
							COERCE_EXPLICIT_CAST, -1, false);
}

/*
 * strip_implicit_coercions: remove implicit coercions at top level of tree
 *
 * This doesn't modify or copy the input expression tree, just return a
 * pointer to a suitable place within it.
 *
 * Note: there isn't any useful thing we can do with a RowExpr here, so
 * just return it unchanged, even if it's marked as an implicit coercion.
 */
Node *
strip_implicit_coercions(Node *node)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, FuncExpr))
	{
		FuncExpr   *f = (FuncExpr *) node;

		if (f->funcformat == COERCE_IMPLICIT_CAST)
			return strip_implicit_coercions(linitial(f->args));
	}
	else if (IsA(node, RelabelType))
	{
		RelabelType *r = (RelabelType *) node;

		if (r->relabelformat == COERCE_IMPLICIT_CAST)
			return strip_implicit_coercions((Node *) r->arg);
	}
	else if (IsA(node, CoerceViaIO))
	{
		CoerceViaIO *c = (CoerceViaIO *) node;

		if (c->coerceformat == COERCE_IMPLICIT_CAST)
			return strip_implicit_coercions((Node *) c->arg);
	}
	else if (IsA(node, ArrayCoerceExpr))
	{
		ArrayCoerceExpr *c = (ArrayCoerceExpr *) node;

		if (c->coerceformat == COERCE_IMPLICIT_CAST)
			return strip_implicit_coercions((Node *) c->arg);
	}
	else if (IsA(node, ConvertRowtypeExpr))
	{
		ConvertRowtypeExpr *c = (ConvertRowtypeExpr *) node;

		if (c->convertformat == COERCE_IMPLICIT_CAST)
			return strip_implicit_coercions((Node *) c->arg);
	}
	else if (IsA(node, CoerceToDomain))
	{
		CoerceToDomain *c = (CoerceToDomain *) node;

		if (c->coercionformat == COERCE_IMPLICIT_CAST)
			return strip_implicit_coercions((Node *) c->arg);
	}
	return node;
}

/*
 * expression_returns_set
 *	  Test whether an expression returns a set result.
 *
 * Because we use expression_tree_walker(), this can also be applied to
 * whole targetlists; it'll produce true if any one of the tlist items
 * returns a set.
 */
bool
expression_returns_set(Node *clause)
{
	return expression_returns_set_walker(clause, NULL);
}

static bool
expression_returns_set_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, FuncExpr))
	{
		FuncExpr   *expr = (FuncExpr *) node;

		if (expr->funcretset)
			return true;
		/* else fall through to check args */
	}
	if (IsA(node, OpExpr))
	{
		OpExpr	   *expr = (OpExpr *) node;

		if (expr->opretset)
			return true;
		/* else fall through to check args */
	}

	/* Avoid recursion for some cases that parser checks not to return a set */
	if (IsA(node, Aggref))
		return false;
	if (IsA(node, GroupingFunc))
		return false;
	if (IsA(node, WindowFunc))
		return false;

	return expression_tree_walker(node, expression_returns_set_walker,
								  context);
}


/*
 *	exprCollation -
 *	  returns the Oid of the collation of the expression's result.
 *
 * Note: expression nodes that can invoke functions generally have an
 * "inputcollid" field, which is what the function should use as collation.
 * That is the resolved common collation of the node's inputs.  It is often
 * but not always the same as the result collation; in particular, if the
 * function produces a non-collatable result type from collatable inputs
 * or vice versa, the two are different.
 */
Oid
exprCollation(const Node *expr)
{
	Oid			coll;

	if (!expr)
		return InvalidOid;

	switch (nodeTag(expr))
	{
		case T_Var:
			coll = ((const Var *) expr)->varcollid;
			break;
		case T_Const:
			coll = ((const Const *) expr)->constcollid;
			break;
		case T_Param:
			coll = ((const Param *) expr)->paramcollid;
			break;
		case T_Aggref:
			coll = ((const Aggref *) expr)->aggcollid;
			break;
		case T_GroupingFunc:
			coll = InvalidOid;
			break;
		case T_WindowFunc:
			coll = ((const WindowFunc *) expr)->wincollid;
			break;
		case T_SubscriptingRef:
			coll = ((const SubscriptingRef *) expr)->refcollid;
			break;
		case T_FuncExpr:
			coll = ((const FuncExpr *) expr)->funccollid;
			break;
		case T_NamedArgExpr:
			coll = exprCollation((Node *) ((const NamedArgExpr *) expr)->arg);
			break;
		case T_OpExpr:
			coll = ((const OpExpr *) expr)->opcollid;
			break;
		case T_DistinctExpr:
			coll = ((const DistinctExpr *) expr)->opcollid;
			break;
		case T_NullIfExpr:
			coll = ((const NullIfExpr *) expr)->opcollid;
			break;
		case T_ScalarArrayOpExpr:
			coll = InvalidOid;	/* result is always boolean */
			break;
		case T_BoolExpr:
			coll = InvalidOid;	/* result is always boolean */
			break;
		case T_SubLink:
			{
				const SubLink *sublink = (const SubLink *) expr;

				if (sublink->subLinkType == EXPR_SUBLINK ||
					sublink->subLinkType == ARRAY_SUBLINK)
				{
					/* get the collation of subselect's first target column */
					Query	   *qtree = (Query *) sublink->subselect;
					TargetEntry *tent;

					if (!qtree || !IsA(qtree, Query))
						elog(ERROR, "cannot get collation for untransformed sublink");
					tent = linitial_node(TargetEntry, qtree->targetList);
					Assert(!tent->resjunk);
					coll = exprCollation((Node *) tent->expr);
					/* collation doesn't change if it's converted to array */
				}
				else
				{
					/* otherwise, result is RECORD or BOOLEAN */
					coll = InvalidOid;
				}
			}
			break;
		case T_SubPlan:
			{
				const SubPlan *subplan = (const SubPlan *) expr;

				if (subplan->subLinkType == EXPR_SUBLINK ||
					subplan->subLinkType == ARRAY_SUBLINK)
				{
					/* get the collation of subselect's first target column */
					coll = subplan->firstColCollation;
					/* collation doesn't change if it's converted to array */
				}
				else
				{
					/* otherwise, result is RECORD or BOOLEAN */
					coll = InvalidOid;
				}
			}
			break;
		case T_AlternativeSubPlan:
			{
				const AlternativeSubPlan *asplan = (const AlternativeSubPlan *) expr;

				/* subplans should all return the same thing */
				coll = exprCollation((Node *) linitial(asplan->subplans));
			}
			break;
		case T_FieldSelect:
			coll = ((const FieldSelect *) expr)->resultcollid;
			break;
		case T_FieldStore:
			coll = InvalidOid;	/* result is always composite */
			break;
		case T_RelabelType:
			coll = ((const RelabelType *) expr)->resultcollid;
			break;
		case T_CoerceViaIO:
			coll = ((const CoerceViaIO *) expr)->resultcollid;
			break;
		case T_ArrayCoerceExpr:
			coll = ((const ArrayCoerceExpr *) expr)->resultcollid;
			break;
		case T_ConvertRowtypeExpr:
			coll = InvalidOid;	/* result is always composite */
			break;
		case T_CollateExpr:
			coll = ((const CollateExpr *) expr)->collOid;
			break;
		case T_CaseExpr:
			coll = ((const CaseExpr *) expr)->casecollid;
			break;
		case T_CaseTestExpr:
			coll = ((const CaseTestExpr *) expr)->collation;
			break;
		case T_ArrayExpr:
			coll = ((const ArrayExpr *) expr)->array_collid;
			break;
		case T_RowExpr:
			coll = InvalidOid;	/* result is always composite */
			break;
		case T_RowCompareExpr:
			coll = InvalidOid;	/* result is always boolean */
			break;
		case T_CoalesceExpr:
			coll = ((const CoalesceExpr *) expr)->coalescecollid;
			break;
		case T_MinMaxExpr:
			coll = ((const MinMaxExpr *) expr)->minmaxcollid;
			break;
		case T_SQLValueFunction:
			/* Returns either NAME or a non-collatable type */
			if (((const SQLValueFunction *) expr)->type == NAMEOID)
				coll = C_COLLATION_OID;
			else
				coll = InvalidOid;
			break;
		case T_XmlExpr:

			/*
			 * XMLSERIALIZE returns text from non-collatable inputs, so its
			 * collation is always default.  The other cases return boolean or
			 * XML, which are non-collatable.
			 */
			if (((const XmlExpr *) expr)->op == IS_XMLSERIALIZE)
				coll = DEFAULT_COLLATION_OID;
			else
				coll = InvalidOid;
			break;
		case T_NullTest:
			coll = InvalidOid;	/* result is always boolean */
			break;
		case T_BooleanTest:
			coll = InvalidOid;	/* result is always boolean */
			break;
		case T_CoerceToDomain:
			coll = ((const CoerceToDomain *) expr)->resultcollid;
			break;
		case T_CoerceToDomainValue:
			coll = ((const CoerceToDomainValue *) expr)->collation;
			break;
		case T_SetToDefault:
			coll = ((const SetToDefault *) expr)->collation;
			break;
		case T_CurrentOfExpr:
			coll = InvalidOid;	/* result is always boolean */
			break;
		case T_NextValueExpr:
			coll = InvalidOid;	/* result is always an integer type */
			break;
		case T_InferenceElem:
			coll = exprCollation((Node *) ((const InferenceElem *) expr)->expr);
			break;
		case T_PlaceHolderVar:
			coll = exprCollation((Node *) ((const PlaceHolderVar *) expr)->phexpr);
			break;
		default:
			elog(ERROR, "unrecognized node type: %d", (int) nodeTag(expr));
			coll = InvalidOid;	/* keep compiler quiet */
			break;
	}
	return coll;
}

/*
 *	exprInputCollation -
 *	  returns the Oid of the collation a function should use, if available.
 *
 * Result is InvalidOid if the node type doesn't store this information.
 */
Oid
exprInputCollation(const Node *expr)
{
	Oid			coll;

	if (!expr)
		return InvalidOid;

	switch (nodeTag(expr))
	{
		case T_Aggref:
			coll = ((const Aggref *) expr)->inputcollid;
			break;
		case T_WindowFunc:
			coll = ((const WindowFunc *) expr)->inputcollid;
			break;
		case T_FuncExpr:
			coll = ((const FuncExpr *) expr)->inputcollid;
			break;
		case T_OpExpr:
			coll = ((const OpExpr *) expr)->inputcollid;
			break;
		case T_DistinctExpr:
			coll = ((const DistinctExpr *) expr)->inputcollid;
			break;
		case T_NullIfExpr:
			coll = ((const NullIfExpr *) expr)->inputcollid;
			break;
		case T_ScalarArrayOpExpr:
			coll = ((const ScalarArrayOpExpr *) expr)->inputcollid;
			break;
		case T_MinMaxExpr:
			coll = ((const MinMaxExpr *) expr)->inputcollid;
			break;
		default:
			coll = InvalidOid;
			break;
	}
	return coll;
}

/*
 *	exprSetCollation -
 *	  Assign collation information to an expression tree node.
 *
 * Note: since this is only used during parse analysis, we don't need to
 * worry about subplans or PlaceHolderVars.
 */
void
exprSetCollation(Node *expr, Oid collation)
{
	switch (nodeTag(expr))
	{
		case T_Var:
			((Var *) expr)->varcollid = collation;
			break;
		case T_Const:
			((Const *) expr)->constcollid = collation;
			break;
		case T_Param:
			((Param *) expr)->paramcollid = collation;
			break;
		case T_Aggref:
			((Aggref *) expr)->aggcollid = collation;
			break;
		case T_GroupingFunc:
			Assert(!OidIsValid(collation));
			break;
		case T_WindowFunc:
			((WindowFunc *) expr)->wincollid = collation;
			break;
		case T_SubscriptingRef:
			((SubscriptingRef *) expr)->refcollid = collation;
			break;
		case T_FuncExpr:
			((FuncExpr *) expr)->funccollid = collation;
			break;
		case T_NamedArgExpr:
			Assert(collation == exprCollation((Node *) ((NamedArgExpr *) expr)->arg));
			break;
		case T_OpExpr:
			((OpExpr *) expr)->opcollid = collation;
			break;
		case T_DistinctExpr:
			((DistinctExpr *) expr)->opcollid = collation;
			break;
		case T_NullIfExpr:
			((NullIfExpr *) expr)->opcollid = collation;
			break;
		case T_ScalarArrayOpExpr:
			Assert(!OidIsValid(collation)); /* result is always boolean */
			break;
		case T_BoolExpr:
			Assert(!OidIsValid(collation)); /* result is always boolean */
			break;
		case T_SubLink:
#ifdef USE_ASSERT_CHECKING
			{
				SubLink    *sublink = (SubLink *) expr;

				if (sublink->subLinkType == EXPR_SUBLINK ||
					sublink->subLinkType == ARRAY_SUBLINK)
				{
					/* get the collation of subselect's first target column */
					Query	   *qtree = (Query *) sublink->subselect;
					TargetEntry *tent;

					if (!qtree || !IsA(qtree, Query))
						elog(ERROR, "cannot set collation for untransformed sublink");
					tent = linitial_node(TargetEntry, qtree->targetList);
					Assert(!tent->resjunk);
					Assert(collation == exprCollation((Node *) tent->expr));
				}
				else
				{
					/* otherwise, result is RECORD or BOOLEAN */
					Assert(!OidIsValid(collation));
				}
			}
#endif							/* USE_ASSERT_CHECKING */
			break;
		case T_FieldSelect:
			((FieldSelect *) expr)->resultcollid = collation;
			break;
		case T_FieldStore:
			Assert(!OidIsValid(collation)); /* result is always composite */
			break;
		case T_RelabelType:
			((RelabelType *) expr)->resultcollid = collation;
			break;
		case T_CoerceViaIO:
			((CoerceViaIO *) expr)->resultcollid = collation;
			break;
		case T_ArrayCoerceExpr:
			((ArrayCoerceExpr *) expr)->resultcollid = collation;
			break;
		case T_ConvertRowtypeExpr:
			Assert(!OidIsValid(collation)); /* result is always composite */
			break;
		case T_CaseExpr:
			((CaseExpr *) expr)->casecollid = collation;
			break;
		case T_ArrayExpr:
			((ArrayExpr *) expr)->array_collid = collation;
			break;
		case T_RowExpr:
			Assert(!OidIsValid(collation)); /* result is always composite */
			break;
		case T_RowCompareExpr:
			Assert(!OidIsValid(collation)); /* result is always boolean */
			break;
		case T_CoalesceExpr:
			((CoalesceExpr *) expr)->coalescecollid = collation;
			break;
		case T_MinMaxExpr:
			((MinMaxExpr *) expr)->minmaxcollid = collation;
			break;
		case T_SQLValueFunction:
			Assert((((SQLValueFunction *) expr)->type == NAMEOID) ?
				   (collation == C_COLLATION_OID) :
				   (collation == InvalidOid));
			break;
		case T_XmlExpr:
			Assert((((XmlExpr *) expr)->op == IS_XMLSERIALIZE) ?
				   (collation == DEFAULT_COLLATION_OID) :
				   (collation == InvalidOid));
			break;
		case T_NullTest:
			Assert(!OidIsValid(collation)); /* result is always boolean */
			break;
		case T_BooleanTest:
			Assert(!OidIsValid(collation)); /* result is always boolean */
			break;
		case T_CoerceToDomain:
			((CoerceToDomain *) expr)->resultcollid = collation;
			break;
		case T_CoerceToDomainValue:
			((CoerceToDomainValue *) expr)->collation = collation;
			break;
		case T_SetToDefault:
			((SetToDefault *) expr)->collation = collation;
			break;
		case T_CurrentOfExpr:
			Assert(!OidIsValid(collation)); /* result is always boolean */
			break;
		case T_NextValueExpr:
			Assert(!OidIsValid(collation)); /* result is always an integer
											 * type */
			break;
		default:
			elog(ERROR, "unrecognized node type: %d", (int) nodeTag(expr));
			break;
	}
}

/*
 *	exprSetInputCollation -
 *	  Assign input-collation information to an expression tree node.
 *
 * This is a no-op for node types that don't store their input collation.
 * Note we omit RowCompareExpr, which needs special treatment since it
 * contains multiple input collation OIDs.
 */
void
exprSetInputCollation(Node *expr, Oid inputcollation)
{
	switch (nodeTag(expr))
	{
		case T_Aggref:
			((Aggref *) expr)->inputcollid = inputcollation;
			break;
		case T_WindowFunc:
			((WindowFunc *) expr)->inputcollid = inputcollation;
			break;
		case T_FuncExpr:
			((FuncExpr *) expr)->inputcollid = inputcollation;
			break;
		case T_OpExpr:
			((OpExpr *) expr)->inputcollid = inputcollation;
			break;
		case T_DistinctExpr:
			((DistinctExpr *) expr)->inputcollid = inputcollation;
			break;
		case T_NullIfExpr:
			((NullIfExpr *) expr)->inputcollid = inputcollation;
			break;
		case T_ScalarArrayOpExpr:
			((ScalarArrayOpExpr *) expr)->inputcollid = inputcollation;
			break;
		case T_MinMaxExpr:
			((MinMaxExpr *) expr)->inputcollid = inputcollation;
			break;
		default:
			break;
	}
}


/*
 *	exprLocation -
 *	  returns the parse location of an expression tree, for error reports
 *
 * -1 is returned if the location can't be determined.
 *
 * For expressions larger than a single token, the intent here is to
 * return the location of the expression's leftmost token, not necessarily
 * the topmost Node's location field.  For example, an OpExpr's location
 * field will point at the operator name, but if it is not a prefix operator
 * then we should return the location of the left-hand operand instead.
 * The reason is that we want to reference the entire expression not just
 * that operator, and pointing to its start seems to be the most natural way.
 *
 * The location is not perfect --- for example, since the grammar doesn't
 * explicitly represent parentheses in the parsetree, given something that
 * had been written "(a + b) * c" we are going to point at "a" not "(".
 * But it should be plenty good enough for error reporting purposes.
 *
 * You might think that this code is overly general, for instance why check
 * the operands of a FuncExpr node, when the function name can be expected
 * to be to the left of them?  There are a couple of reasons.  The grammar
 * sometimes builds expressions that aren't quite what the user wrote;
 * for instance x IS NOT BETWEEN ... becomes a NOT-expression whose keyword
 * pointer is to the right of its leftmost argument.  Also, nodes that were
 * inserted implicitly by parse analysis (such as FuncExprs for implicit
 * coercions) will have location -1, and so we can have odd combinations of
 * known and unknown locations in a tree.
 */
int
exprLocation(const Node *expr)
{
	int			loc;

	if (expr == NULL)
		return -1;
	switch (nodeTag(expr))
	{
		case T_RangeVar:
			loc = ((const RangeVar *) expr)->location;
			break;
		case T_TableFunc:
			loc = ((const TableFunc *) expr)->location;
			break;
		case T_Var:
			loc = ((const Var *) expr)->location;
			break;
		case T_Const:
			loc = ((const Const *) expr)->location;
			break;
		case T_Param:
			loc = ((const Param *) expr)->location;
			break;
		case T_Aggref:
			/* function name should always be the first thing */
			loc = ((const Aggref *) expr)->location;
			break;
		case T_GroupingFunc:
			loc = ((const GroupingFunc *) expr)->location;
			break;
		case T_WindowFunc:
			/* function name should always be the first thing */
			loc = ((const WindowFunc *) expr)->location;
			break;
		case T_SubscriptingRef:
			/* just use container argument's location */
			loc = exprLocation((Node *) ((const SubscriptingRef *) expr)->refexpr);
			break;
		case T_FuncExpr:
			{
				const FuncExpr *fexpr = (const FuncExpr *) expr;

				/* consider both function name and leftmost arg */
				loc = leftmostLoc(fexpr->location,
								  exprLocation((Node *) fexpr->args));
			}
			break;
		case T_NamedArgExpr:
			{
				const NamedArgExpr *na = (const NamedArgExpr *) expr;

				/* consider both argument name and value */
				loc = leftmostLoc(na->location,
								  exprLocation((Node *) na->arg));
			}
			break;
		case T_OpExpr:
		case T_DistinctExpr:	/* struct-equivalent to OpExpr */
		case T_NullIfExpr:		/* struct-equivalent to OpExpr */
			{
				const OpExpr *opexpr = (const OpExpr *) expr;

				/* consider both operator name and leftmost arg */
				loc = leftmostLoc(opexpr->location,
								  exprLocation((Node *) opexpr->args));
			}
			break;
		case T_ScalarArrayOpExpr:
			{
				const ScalarArrayOpExpr *saopexpr = (const ScalarArrayOpExpr *) expr;

				/* consider both operator name and leftmost arg */
				loc = leftmostLoc(saopexpr->location,
								  exprLocation((Node *) saopexpr->args));
			}
			break;
		case T_BoolExpr:
			{
				const BoolExpr *bexpr = (const BoolExpr *) expr;

				/*
				 * Same as above, to handle either NOT or AND/OR.  We can't
				 * special-case NOT because of the way that it's used for
				 * things like IS NOT BETWEEN.
				 */
				loc = leftmostLoc(bexpr->location,
								  exprLocation((Node *) bexpr->args));
			}
			break;
		case T_SubLink:
			{
				const SubLink *sublink = (const SubLink *) expr;

				/* check the testexpr, if any, and the operator/keyword */
				loc = leftmostLoc(exprLocation(sublink->testexpr),
								  sublink->location);
			}
			break;
		case T_FieldSelect:
			/* just use argument's location */
			loc = exprLocation((Node *) ((const FieldSelect *) expr)->arg);
			break;
		case T_FieldStore:
			/* just use argument's location */
			loc = exprLocation((Node *) ((const FieldStore *) expr)->arg);
			break;
		case T_RelabelType:
			{
				const RelabelType *rexpr = (const RelabelType *) expr;

				/* Much as above */
				loc = leftmostLoc(rexpr->location,
								  exprLocation((Node *) rexpr->arg));
			}
			break;
		case T_CoerceViaIO:
			{
				const CoerceViaIO *cexpr = (const CoerceViaIO *) expr;

				/* Much as above */
				loc = leftmostLoc(cexpr->location,
								  exprLocation((Node *) cexpr->arg));
			}
			break;
		case T_ArrayCoerceExpr:
			{
				const ArrayCoerceExpr *cexpr = (const ArrayCoerceExpr *) expr;

				/* Much as above */
				loc = leftmostLoc(cexpr->location,
								  exprLocation((Node *) cexpr->arg));
			}
			break;
		case T_ConvertRowtypeExpr:
			{
				const ConvertRowtypeExpr *cexpr = (const ConvertRowtypeExpr *) expr;

				/* Much as above */
				loc = leftmostLoc(cexpr->location,
								  exprLocation((Node *) cexpr->arg));
			}
			break;
		case T_CollateExpr:
			/* just use argument's location */
			loc = exprLocation((Node *) ((const CollateExpr *) expr)->arg);
			break;
		case T_CaseExpr:
			/* CASE keyword should always be the first thing */
			loc = ((const CaseExpr *) expr)->location;
			break;
		case T_CaseWhen:
			/* WHEN keyword should always be the first thing */
			loc = ((const CaseWhen *) expr)->location;
			break;
		case T_ArrayExpr:
			/* the location points at ARRAY or [, which must be leftmost */
			loc = ((const ArrayExpr *) expr)->location;
			break;
		case T_RowExpr:
			/* the location points at ROW or (, which must be leftmost */
			loc = ((const RowExpr *) expr)->location;
			break;
		case T_RowCompareExpr:
			/* just use leftmost argument's location */
			loc = exprLocation((Node *) ((const RowCompareExpr *) expr)->largs);
			break;
		case T_CoalesceExpr:
			/* COALESCE keyword should always be the first thing */
			loc = ((const CoalesceExpr *) expr)->location;
			break;
		case T_MinMaxExpr:
			/* GREATEST/LEAST keyword should always be the first thing */
			loc = ((const MinMaxExpr *) expr)->location;
			break;
		case T_SQLValueFunction:
			/* function keyword should always be the first thing */
			loc = ((const SQLValueFunction *) expr)->location;
			break;
		case T_XmlExpr:
			{
				const XmlExpr *xexpr = (const XmlExpr *) expr;

				/* consider both function name and leftmost arg */
				loc = leftmostLoc(xexpr->location,
								  exprLocation((Node *) xexpr->args));
			}
			break;
		case T_NullTest:
			{
				const NullTest *nexpr = (const NullTest *) expr;

				/* Much as above */
				loc = leftmostLoc(nexpr->location,
								  exprLocation((Node *) nexpr->arg));
			}
			break;
		case T_BooleanTest:
			{
				const BooleanTest *bexpr = (const BooleanTest *) expr;

				/* Much as above */
				loc = leftmostLoc(bexpr->location,
								  exprLocation((Node *) bexpr->arg));
			}
			break;
		case T_CoerceToDomain:
			{
				const CoerceToDomain *cexpr = (const CoerceToDomain *) expr;

				/* Much as above */
				loc = leftmostLoc(cexpr->location,
								  exprLocation((Node *) cexpr->arg));
			}
			break;
		case T_CoerceToDomainValue:
			loc = ((const CoerceToDomainValue *) expr)->location;
			break;
		case T_SetToDefault:
			loc = ((const SetToDefault *) expr)->location;
			break;
		case T_TargetEntry:
			/* just use argument's location */
			loc = exprLocation((Node *) ((const TargetEntry *) expr)->expr);
			break;
		case T_IntoClause:
			/* use the contained RangeVar's location --- close enough */
			loc = exprLocation((Node *) ((const IntoClause *) expr)->rel);
			break;
		case T_List:
			{
				/* report location of first list member that has a location */
				ListCell   *lc;

				loc = -1;		/* just to suppress compiler warning */
				foreach(lc, (const List *) expr)
				{
					loc = exprLocation((Node *) lfirst(lc));
					if (loc >= 0)
						break;
				}
			}
			break;
		case T_A_Expr:
			{
				const A_Expr *aexpr = (const A_Expr *) expr;

				/* use leftmost of operator or left operand (if any) */
				/* we assume right operand can't be to left of operator */
				loc = leftmostLoc(aexpr->location,
								  exprLocation(aexpr->lexpr));
			}
			break;
		case T_ColumnRef:
			loc = ((const ColumnRef *) expr)->location;
			break;
		case T_ParamRef:
			loc = ((const ParamRef *) expr)->location;
			break;
		case T_A_Const:
			loc = ((const A_Const *) expr)->location;
			break;
		case T_FuncCall:
			{
				const FuncCall *fc = (const FuncCall *) expr;

				/* consider both function name and leftmost arg */
				/* (we assume any ORDER BY nodes must be to right of name) */
				loc = leftmostLoc(fc->location,
								  exprLocation((Node *) fc->args));
			}
			break;
		case T_A_ArrayExpr:
			/* the location points at ARRAY or [, which must be leftmost */
			loc = ((const A_ArrayExpr *) expr)->location;
			break;
		case T_ResTarget:
			/* we need not examine the contained expression (if any) */
			loc = ((const ResTarget *) expr)->location;
			break;
		case T_MultiAssignRef:
			loc = exprLocation(((const MultiAssignRef *) expr)->source);
			break;
		case T_TypeCast:
			{
				const TypeCast *tc = (const TypeCast *) expr;

				/*
				 * This could represent CAST(), ::, or TypeName 'literal', so
				 * any of the components might be leftmost.
				 */
				loc = exprLocation(tc->arg);
				loc = leftmostLoc(loc, tc->typeName->location);
				loc = leftmostLoc(loc, tc->location);
			}
			break;
		case T_CollateClause:
			/* just use argument's location */
			loc = exprLocation(((const CollateClause *) expr)->arg);
			break;
		case T_SortBy:
			/* just use argument's location (ignore operator, if any) */
			loc = exprLocation(((const SortBy *) expr)->node);
			break;
		case T_WindowDef:
			loc = ((const WindowDef *) expr)->location;
			break;
		case T_RangeTableSample:
			loc = ((const RangeTableSample *) expr)->location;
			break;
		case T_TypeName:
			loc = ((const TypeName *) expr)->location;
			break;
		case T_ColumnDef:
			loc = ((const ColumnDef *) expr)->location;
			break;
		case T_Constraint:
			loc = ((const Constraint *) expr)->location;
			break;
		case T_FunctionParameter:
			/* just use typename's location */
			loc = exprLocation((Node *) ((const FunctionParameter *) expr)->argType);
			break;
		case T_XmlSerialize:
			/* XMLSERIALIZE keyword should always be the first thing */
			loc = ((const XmlSerialize *) expr)->location;
			break;
		case T_GroupingSet:
			loc = ((const GroupingSet *) expr)->location;
			break;
		case T_WithClause:
			loc = ((const WithClause *) expr)->location;
			break;
		case T_InferClause:
			loc = ((const InferClause *) expr)->location;
			break;
		case T_OnConflictClause:
			loc = ((const OnConflictClause *) expr)->location;
			break;
		case T_CommonTableExpr:
			loc = ((const CommonTableExpr *) expr)->location;
			break;
		case T_PlaceHolderVar:
			/* just use argument's location */
			loc = exprLocation((Node *) ((const PlaceHolderVar *) expr)->phexpr);
			break;
		case T_InferenceElem:
			/* just use nested expr's location */
			loc = exprLocation((Node *) ((const InferenceElem *) expr)->expr);
			break;
		case T_PartitionElem:
			loc = ((const PartitionElem *) expr)->location;
			break;
		case T_PartitionSpec:
			loc = ((const PartitionSpec *) expr)->location;
			break;
		case T_PartitionBoundSpec:
			loc = ((const PartitionBoundSpec *) expr)->location;
			break;
		case T_PartitionRangeDatum:
			loc = ((const PartitionRangeDatum *) expr)->location;
			break;
		default:
			/* for any other node type it's just unknown... */
			loc = -1;
			break;
	}
	return loc;
}

/*
 * leftmostLoc - support for exprLocation
 *
 * Take the minimum of two parse location values, but ignore unknowns
 */
static int
leftmostLoc(int loc1, int loc2)
{
	if (loc1 < 0)
		return loc2;
	else if (loc2 < 0)
		return loc1;
	else
		return Min(loc1, loc2);
}


/*
 * fix_opfuncids
 *	  Calculate opfuncid field from opno for each OpExpr node in given tree.
 *	  The given tree can be anything expression_tree_walker handles.
 *
 * The argument is modified in-place.  (This is OK since we'd want the
 * same change for any node, even if it gets visited more than once due to
 * shared structure.)
 */
void
fix_opfuncids(Node *node)
{
	/* This tree walk requires no special setup, so away we go... */
	fix_opfuncids_walker(node, NULL);
}

static bool
fix_opfuncids_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, OpExpr))
		set_opfuncid((OpExpr *) node);
	else if (IsA(node, DistinctExpr))
		set_opfuncid((OpExpr *) node);	/* rely on struct equivalence */
	else if (IsA(node, NullIfExpr))
		set_opfuncid((OpExpr *) node);	/* rely on struct equivalence */
	else if (IsA(node, ScalarArrayOpExpr))
		set_sa_opfuncid((ScalarArrayOpExpr *) node);
	return expression_tree_walker(node, fix_opfuncids_walker, context);
}

/*
 * set_opfuncid
 *		Set the opfuncid (procedure OID) in an OpExpr node,
 *		if it hasn't been set already.
 *
 * Because of struct equivalence, this can also be used for
 * DistinctExpr and NullIfExpr nodes.
 */
void
set_opfuncid(OpExpr *opexpr)
{
	if (opexpr->opfuncid == InvalidOid)
		opexpr->opfuncid = get_opcode(opexpr->opno);
}

/*
 * set_sa_opfuncid
 *		As above, for ScalarArrayOpExpr nodes.
 */
void
set_sa_opfuncid(ScalarArrayOpExpr *opexpr)
{
	if (opexpr->opfuncid == InvalidOid)
		opexpr->opfuncid = get_opcode(opexpr->opno);
}


/*
 *	check_functions_in_node -
 *	  apply checker() to each function OID contained in given expression node
 *
 * Returns true if the checker() function does; for nodes representing more
 * than one function call, returns true if the checker() function does so
 * for any of those functions.  Returns false if node does not invoke any
 * SQL-visible function.  Caller must not pass node == NULL.
 *
 * This function examines only the given node; it does not recurse into any
 * sub-expressions.  Callers typically prefer to keep control of the recursion
 * for themselves, in case additional checks should be made, or because they
 * have special rules about which parts of the tree need to be visited.
 *
 * Note: we ignore MinMaxExpr, SQLValueFunction, XmlExpr, CoerceToDomain,
 * and NextValueExpr nodes, because they do not contain SQL function OIDs.
 * However, they can invoke SQL-visible functions, so callers should take
 * thought about how to treat them.
 */
bool
check_functions_in_node(Node *node, check_function_callback checker,
						void *context)
{
	switch (nodeTag(node))
	{
		case T_Aggref:
			{
				Aggref	   *expr = (Aggref *) node;

				if (checker(expr->aggfnoid, context))
					return true;
			}
			break;
		case T_WindowFunc:
			{
				WindowFunc *expr = (WindowFunc *) node;

				if (checker(expr->winfnoid, context))
					return true;
			}
			break;
		case T_FuncExpr:
			{
				FuncExpr   *expr = (FuncExpr *) node;

				if (checker(expr->funcid, context))
					return true;
			}
			break;
		case T_OpExpr:
		case T_DistinctExpr:	/* struct-equivalent to OpExpr */
		case T_NullIfExpr:		/* struct-equivalent to OpExpr */
			{
				OpExpr	   *expr = (OpExpr *) node;

				/* Set opfuncid if it wasn't set already */
				set_opfuncid(expr);
				if (checker(expr->opfuncid, context))
					return true;
			}
			break;
		case T_ScalarArrayOpExpr:
			{
				ScalarArrayOpExpr *expr = (ScalarArrayOpExpr *) node;

				set_sa_opfuncid(expr);
				if (checker(expr->opfuncid, context))
					return true;
			}
			break;
		case T_CoerceViaIO:
			{
				CoerceViaIO *expr = (CoerceViaIO *) node;
				Oid			iofunc;
				Oid			typioparam;
				bool		typisvarlena;

				/* check the result type's input function */
				getTypeInputInfo(expr->resulttype,
								 &iofunc, &typioparam);
				if (checker(iofunc, context))
					return true;
				/* check the input type's output function */
				getTypeOutputInfo(exprType((Node *) expr->arg),
								  &iofunc, &typisvarlena);
				if (checker(iofunc, context))
					return true;
			}
			break;
		case T_RowCompareExpr:
			{
				RowCompareExpr *rcexpr = (RowCompareExpr *) node;
				ListCell   *opid;

				foreach(opid, rcexpr->opnos)
				{
					Oid			opfuncid = get_opcode(lfirst_oid(opid));

					if (checker(opfuncid, context))
						return true;
				}
			}
			break;
		default:
			break;
	}
	return false;
}


/*
 * Standard expression-tree walking support
 *
 * We used to have near-duplicate code in many different routines that
 * understood how to recurse through an expression node tree.  That was
 * a pain to maintain, and we frequently had bugs due to some particular
 * routine neglecting to support a particular node type.  In most cases,
 * these routines only actually care about certain node types, and don't
 * care about other types except insofar as they have to recurse through
 * non-primitive node types.  Therefore, we now provide generic tree-walking
 * logic to consolidate the redundant "boilerplate" code.  There are
 * two versions: expression_tree_walker() and expression_tree_mutator().
 */

/*
 * expression_tree_walker() is designed to support routines that traverse
 * a tree in a read-only fashion (although it will also work for routines
 * that modify nodes in-place but never add/delete/replace nodes).
 * A walker routine should look like this:
 *
 * bool my_walker (Node *node, my_struct *context)
 * {
 *		if (node == NULL)
 *			return false;
 *		// check for nodes that special work is required for, eg:
 *		if (IsA(node, Var))
 *		{
 *			... do special actions for Var nodes
 *		}
 *		else if (IsA(node, ...))
 *		{
 *			... do special actions for other node types
 *		}
 *		// for any node type not specially processed, do:
 *		return expression_tree_walker(node, my_walker, (void *) context);
 * }
 *
 * The "context" argument points to a struct that holds whatever context
 * information the walker routine needs --- it can be used to return data
 * gathered by the walker, too.  This argument is not touched by
 * expression_tree_walker, but it is passed down to recursive sub-invocations
 * of my_walker.  The tree walk is started from a setup routine that
 * fills in the appropriate context struct, calls my_walker with the top-level
 * node of the tree, and then examines the results.
 *
 * The walker routine should return "false" to continue the tree walk, or
 * "true" to abort the walk and immediately return "true" to the top-level
 * caller.  This can be used to short-circuit the traversal if the walker
 * has found what it came for.  "false" is returned to the top-level caller
 * iff no invocation of the walker returned "true".
 *
 * The node types handled by expression_tree_walker include all those
 * normally found in target lists and qualifier clauses during the planning
 * stage.  In particular, it handles List nodes since a cnf-ified qual clause
 * will have List structure at the top level, and it handles TargetEntry nodes
 * so that a scan of a target list can be handled without additional code.
 * Also, RangeTblRef, FromExpr, JoinExpr, and SetOperationStmt nodes are
 * handled, so that query jointrees and setOperation trees can be processed
 * without additional code.
 *
 * expression_tree_walker will handle SubLink nodes by recursing normally
 * into the "testexpr" subtree (which is an expression belonging to the outer
 * plan).  It will also call the walker on the sub-Query node; however, when
 * expression_tree_walker itself is called on a Query node, it does nothing
 * and returns "false".  The net effect is that unless the walker does
 * something special at a Query node, sub-selects will not be visited during
 * an expression tree walk. This is exactly the behavior wanted in many cases
 * --- and for those walkers that do want to recurse into sub-selects, special
 * behavior is typically needed anyway at the entry to a sub-select (such as
 * incrementing a depth counter). A walker that wants to examine sub-selects
 * should include code along the lines of:
 *
 *		if (IsA(node, Query))
 *		{
 *			adjust context for subquery;
 *			result = query_tree_walker((Query *) node, my_walker, context,
 *									   0); // adjust flags as needed
 *			restore context if needed;
 *			return result;
 *		}
 *
 * query_tree_walker is a convenience routine (see below) that calls the
 * walker on all the expression subtrees of the given Query node.
 *
 * expression_tree_walker will handle SubPlan nodes by recursing normally
 * into the "testexpr" and the "args" list (which are expressions belonging to
 * the outer plan).  It will not touch the completed subplan, however.  Since
 * there is no link to the original Query, it is not possible to recurse into
 * subselects of an already-planned expression tree.  This is OK for current
 * uses, but may need to be revisited in future.
 */

bool
expression_tree_walker(Node *node,
					   bool (*walker) (),
					   void *context)
{
	ListCell   *temp;

	/*
	 * The walker has already visited the current node, and so we need only
	 * recurse into any sub-nodes it has.
	 *
	 * We assume that the walker is not interested in List nodes per se, so
	 * when we expect a List we just recurse directly to self without
	 * bothering to call the walker.
	 */
	if (node == NULL)
		return false;

	/* Guard against stack overflow due to overly complex expressions */
	check_stack_depth();

	switch (nodeTag(node))
	{
		case T_Var:
		case T_Const:
		case T_Param:
		case T_CaseTestExpr:
		case T_SQLValueFunction:
		case T_CoerceToDomainValue:
		case T_SetToDefault:
		case T_CurrentOfExpr:
		case T_NextValueExpr:
		case T_RangeTblRef:
		case T_SortGroupClause:
			/* primitive node types with no expression subnodes */
			break;
		case T_WithCheckOption:
			return walker(((WithCheckOption *) node)->qual, context);
		case T_Aggref:
			{
				Aggref	   *expr = (Aggref *) node;

				/* recurse directly on List */
				if (expression_tree_walker((Node *) expr->aggdirectargs,
										   walker, context))
					return true;
				if (expression_tree_walker((Node *) expr->args,
										   walker, context))
					return true;
				if (expression_tree_walker((Node *) expr->aggorder,
										   walker, context))
					return true;
				if (expression_tree_walker((Node *) expr->aggdistinct,
										   walker, context))
					return true;
				if (walker((Node *) expr->aggfilter, context))
					return true;
			}
			break;
		case T_GroupingFunc:
			{
				GroupingFunc *grouping = (GroupingFunc *) node;

				if (expression_tree_walker((Node *) grouping->args,
										   walker, context))
					return true;
			}
			break;
		case T_WindowFunc:
			{
				WindowFunc *expr = (WindowFunc *) node;

				/* recurse directly on List */
				if (expression_tree_walker((Node *) expr->args,
										   walker, context))
					return true;
				if (walker((Node *) expr->aggfilter, context))
					return true;
			}
			break;
		case T_SubscriptingRef:
			{
				SubscriptingRef *sbsref = (SubscriptingRef *) node;

				/* recurse directly for upper/lower container index lists */
				if (expression_tree_walker((Node *) sbsref->refupperindexpr,
										   walker, context))
					return true;
				if (expression_tree_walker((Node *) sbsref->reflowerindexpr,
										   walker, context))
					return true;
				/* walker must see the refexpr and refassgnexpr, however */
				if (walker(sbsref->refexpr, context))
					return true;

				if (walker(sbsref->refassgnexpr, context))
					return true;
			}
			break;
		case T_FuncExpr:
			{
				FuncExpr   *expr = (FuncExpr *) node;

				if (expression_tree_walker((Node *) expr->args,
										   walker, context))
					return true;
			}
			break;
		case T_NamedArgExpr:
			return walker(((NamedArgExpr *) node)->arg, context);
		case T_OpExpr:
		case T_DistinctExpr:	/* struct-equivalent to OpExpr */
		case T_NullIfExpr:		/* struct-equivalent to OpExpr */
			{
				OpExpr	   *expr = (OpExpr *) node;

				if (expression_tree_walker((Node *) expr->args,
										   walker, context))
					return true;
			}
			break;
		case T_ScalarArrayOpExpr:
			{
				ScalarArrayOpExpr *expr = (ScalarArrayOpExpr *) node;

				if (expression_tree_walker((Node *) expr->args,
										   walker, context))
					return true;
			}
			break;
		case T_BoolExpr:
			{
				BoolExpr   *expr = (BoolExpr *) node;

				if (expression_tree_walker((Node *) expr->args,
										   walker, context))
					return true;
			}
			break;
		case T_SubLink:
			{
				SubLink    *sublink = (SubLink *) node;

				if (walker(sublink->testexpr, context))
					return true;

				/*
				 * Also invoke the walker on the sublink's Query node, so it
				 * can recurse into the sub-query if it wants to.
				 */
				return walker(sublink->subselect, context);
			}
			break;
		case T_SubPlan:
			{
				SubPlan    *subplan = (SubPlan *) node;

				/* recurse into the testexpr, but not into the Plan */
				if (walker(subplan->testexpr, context))
					return true;
				/* also examine args list */
				if (expression_tree_walker((Node *) subplan->args,
										   walker, context))
					return true;
			}
			break;
		case T_AlternativeSubPlan:
			return walker(((AlternativeSubPlan *) node)->subplans, context);
		case T_FieldSelect:
			return walker(((FieldSelect *) node)->arg, context);
		case T_FieldStore:
			{
				FieldStore *fstore = (FieldStore *) node;

				if (walker(fstore->arg, context))
					return true;
				if (walker(fstore->newvals, context))
					return true;
			}
			break;
		case T_RelabelType:
			return walker(((RelabelType *) node)->arg, context);
		case T_CoerceViaIO:
			return walker(((CoerceViaIO *) node)->arg, context);
		case T_ArrayCoerceExpr:
			{
				ArrayCoerceExpr *acoerce = (ArrayCoerceExpr *) node;

				if (walker(acoerce->arg, context))
					return true;
				if (walker(acoerce->elemexpr, context))
					return true;
			}
			break;
		case T_ConvertRowtypeExpr:
			return walker(((ConvertRowtypeExpr *) node)->arg, context);
		case T_CollateExpr:
			return walker(((CollateExpr *) node)->arg, context);
		case T_CaseExpr:
			{
				CaseExpr   *caseexpr = (CaseExpr *) node;

				if (walker(caseexpr->arg, context))
					return true;
				/* we assume walker doesn't care about CaseWhens, either */
				foreach(temp, caseexpr->args)
				{
					CaseWhen   *when = lfirst_node(CaseWhen, temp);

					if (walker(when->expr, context))
						return true;
					if (walker(when->result, context))
						return true;
				}
				if (walker(caseexpr->defresult, context))
					return true;
			}
			break;
		case T_ArrayExpr:
			return walker(((ArrayExpr *) node)->elements, context);
		case T_RowExpr:
			/* Assume colnames isn't interesting */
			return walker(((RowExpr *) node)->args, context);
		case T_RowCompareExpr:
			{
				RowCompareExpr *rcexpr = (RowCompareExpr *) node;

				if (walker(rcexpr->largs, context))
					return true;
				if (walker(rcexpr->rargs, context))
					return true;
			}
			break;
		case T_CoalesceExpr:
			return walker(((CoalesceExpr *) node)->args, context);
		case T_MinMaxExpr:
			return walker(((MinMaxExpr *) node)->args, context);
		case T_XmlExpr:
			{
				XmlExpr    *xexpr = (XmlExpr *) node;

				if (walker(xexpr->named_args, context))
					return true;
				/* we assume walker doesn't care about arg_names */
				if (walker(xexpr->args, context))
					return true;
			}
			break;
		case T_NullTest:
			return walker(((NullTest *) node)->arg, context);
		case T_BooleanTest:
			return walker(((BooleanTest *) node)->arg, context);
		case T_CoerceToDomain:
			return walker(((CoerceToDomain *) node)->arg, context);
		case T_TargetEntry:
			return walker(((TargetEntry *) node)->expr, context);
		case T_Query:
			/* Do nothing with a sub-Query, per discussion above */
			break;
		case T_WindowClause:
			{
				WindowClause *wc = (WindowClause *) node;

				if (walker(wc->partitionClause, context))
					return true;
				if (walker(wc->orderClause, context))
					return true;
				if (walker(wc->startOffset, context))
					return true;
				if (walker(wc->endOffset, context))
					return true;
			}
			break;
		case T_CommonTableExpr:
			{
				CommonTableExpr *cte = (CommonTableExpr *) node;

				/*
				 * Invoke the walker on the CTE's Query node, so it can
				 * recurse into the sub-query if it wants to.
				 */
				return walker(cte->ctequery, context);
			}
			break;
		case T_List:
			foreach(temp, (List *) node)
			{
				if (walker((Node *) lfirst(temp), context))
					return true;
			}
			break;
		case T_FromExpr:
			{
				FromExpr   *from = (FromExpr *) node;

				if (walker(from->fromlist, context))
					return true;
				if (walker(from->quals, context))
					return true;
			}
			break;
		case T_OnConflictExpr:
			{
				OnConflictExpr *onconflict = (OnConflictExpr *) node;

				if (walker((Node *) onconflict->arbiterElems, context))
					return true;
				if (walker(onconflict->arbiterWhere, context))
					return true;
				if (walker(onconflict->onConflictSet, context))
					return true;
				if (walker(onconflict->onConflictWhere, context))
					return true;
				if (walker(onconflict->exclRelTlist, context))
					return true;
			}
			break;
		case T_PartitionPruneStepOp:
			{
				PartitionPruneStepOp *opstep = (PartitionPruneStepOp *) node;

				if (walker((Node *) opstep->exprs, context))
					return true;
			}
			break;
		case T_PartitionPruneStepCombine:
			/* no expression subnodes */
			break;
		case T_JoinExpr:
			{
				JoinExpr   *join = (JoinExpr *) node;

				if (walker(join->larg, context))
					return true;
				if (walker(join->rarg, context))
					return true;
				if (walker(join->quals, context))
					return true;

				/*
				 * alias clause, using list are deemed uninteresting.
				 */
			}
			break;
		case T_SetOperationStmt:
			{
				SetOperationStmt *setop = (SetOperationStmt *) node;

				if (walker(setop->larg, context))
					return true;
				if (walker(setop->rarg, context))
					return true;

				/* groupClauses are deemed uninteresting */
			}
			break;
		case T_IndexClause:
			{
				IndexClause *iclause = (IndexClause *) node;

				if (walker(iclause->rinfo, context))
					return true;
				if (expression_tree_walker((Node *) iclause->indexquals,
										   walker, context))
					return true;
			}
			break;
		case T_PlaceHolderVar:
			return walker(((PlaceHolderVar *) node)->phexpr, context);
		case T_InferenceElem:
			return walker(((InferenceElem *) node)->expr, context);
		case T_AppendRelInfo:
			{
				AppendRelInfo *appinfo = (AppendRelInfo *) node;

				if (expression_tree_walker((Node *) appinfo->translated_vars,
										   walker, context))
					return true;
			}
			break;
		case T_PlaceHolderInfo:
			return walker(((PlaceHolderInfo *) node)->ph_var, context);
		case T_RangeTblFunction:
			return walker(((RangeTblFunction *) node)->funcexpr, context);
		case T_TableSampleClause:
			{
				TableSampleClause *tsc = (TableSampleClause *) node;

				if (expression_tree_walker((Node *) tsc->args,
										   walker, context))
					return true;
				if (walker((Node *) tsc->repeatable, context))
					return true;
			}
			break;
		case T_TableFunc:
			{
				TableFunc  *tf = (TableFunc *) node;

				if (walker(tf->ns_uris, context))
					return true;
				if (walker(tf->docexpr, context))
					return true;
				if (walker(tf->rowexpr, context))
					return true;
				if (walker(tf->colexprs, context))
					return true;
				if (walker(tf->coldefexprs, context))
					return true;
			}
			break;
		default:
			elog(ERROR, "unrecognized node type: %d",
				 (int) nodeTag(node));
			break;
	}
	return false;
}

/*
 * query_tree_walker --- 启动对 Query 的表达式的遍历
 *
 * 这个例程的存在是为了减少需要知道 Query 中所有表达式子树位置的地方。
 * 注意它可用于在 Query 的顶层启动遍历，无论 walker 是否打算深入子查询。
 * 它也可用于在 walker 内部下降到子查询。
 *
 * 有些调用者希望在子查询中抑制对某些项的访问，通常因为他们需要对这些项
 * 进行特殊处理，或实际上不想递归到子查询。通过 flags 参数的按位或来支持
 * 抑制对指定项的访问。（可以根据需要添加更多的 flag 位。）
 */
bool
query_tree_walker(Query *query,
				  bool (*walker) (),
				  void *context,
				  int flags)
{
	Assert(query != NULL && IsA(query, Query));

	/*
	 * 我们这里不遍历任何 utilityStmt。但是我们不能轻易断言它不存在，
	 * 因为至少有两条代码路径会把来自 CREATE RULE 的动作语句传到这里，
	 * 并且 NOTIFY 在规则动作中是允许的。
	 */

	if (walker((Node *) query->targetList, context))
		return true;
	if (walker((Node *) query->withCheckOptions, context))
		return true;
	if (walker((Node *) query->onConflict, context))
		return true;
	if (walker((Node *) query->returningList, context))
		return true;
	if (walker((Node *) query->jointree, context))
		return true;
	if (walker(query->setOperations, context))
		return true;
	if (walker(query->havingQual, context))
		return true;
	if (walker(query->limitOffset, context))
		return true;
	if (walker(query->limitCount, context))
		return true;

	/*
	 * 大多数调用者不关心 SortGroupClause 节点，因为它们不包含实际的表达式。
	 * 但是它们包含 OID，依赖性遍历器等可能需要这些 OID。
	 */
	if ((flags & QTW_EXAMINE_SORTGROUP))
	{
		if (walker((Node *) query->groupClause, context))
			return true;
		if (walker((Node *) query->windowClause, context))
			return true;
		if (walker((Node *) query->sortClause, context))
			return true;
		if (walker((Node *) query->distinctClause, context))
			return true;
	}
	else
	{
		/*
		 * 但是即便我们对 SortGroupClause 节点不感兴趣，也需要遍历 WindowClause
		 * 节点下的表达式。
		 */
		ListCell   *lc;

		foreach(lc, query->windowClause)
		{
			WindowClause *wc = lfirst_node(WindowClause, lc);

			if (walker(wc->startOffset, context))
				return true;
			if (walker(wc->endOffset, context))
				return true;
		}
	}

	/*
	 * groupingSets 和 rowMarks 不会被遍历：
	 *
	 * groupingSets 仅包含 ressortgrouprefs（整数），在没有对应的 groupClause
	 * 或 tlist 的情况下是没有意义的。因此，任何需要关心它们的 walker 都应
	 * 在其处理 Query 时自行处理。
	 *
	 * rowMarks 不被遍历，因为它仅包含 rangetable 索引（和标志等），因此也应
	 * 在 Query 级别由调用者处理。
	 */

	if (!(flags & QTW_IGNORE_CTE_SUBQUERIES))
	{
		if (walker((Node *) query->cteList, context))
			return true;
	}
	if (!(flags & QTW_IGNORE_RANGE_TABLE))
	{
		if (range_table_walker(query->rtable, walker, context, flags))
			return true;
	}
	return false;
}

/*
 * range_table_walker is just the part of query_tree_walker that scans
 * a query's rangetable.  This is split out since it can be useful on
 * its own.
 *
 * range_table_walker 仅是 query_tree_walker 中用于遍历查询 rangetable 的部分。
 * 将其拆分出来是因为单独使用时也很有用。
 */
bool
range_table_walker(List *rtable,
				   bool (*walker) (),
				   void *context,
				   int flags)
{
	ListCell   *rt;

	foreach(rt, rtable)
	{
		RangeTblEntry *rte = lfirst_node(RangeTblEntry, rt);

		if (range_table_entry_walker(rte, walker, context, flags))
			return true;
	}
	return false;
}

/*
 * Some callers even want to scan the expressions in individual RTEs.
 *
 * 一些调用者甚至希望遍历单个 RTE（RangeTblEntry）中的表达式。
 */
bool
range_table_entry_walker(RangeTblEntry *rte,
						 bool (*walker) (),
						 void *context,
						 int flags)
{
	/*
	 * Walkers might need to examine the RTE node itself either before or
	 * after visiting its contents (or, conceivably, both).  Note that if you
	 * specify neither flag, the walker won't be called on the RTE at all.
	 *
	 * 遍历器可能需要在访问 RTE 内容之前或之后（或两者）检查 RTE 节点本身。
	 * 注意如果既不指定 BEFORE 也不指定 AFTER 标志，遍历器将不会被用于 RTE 本身。
	 */
	if (flags & QTW_EXAMINE_RTES_BEFORE)
		if (walker(rte, context))
			return true;

	switch (rte->rtekind)
	{
		case RTE_RELATION:
			/* RTE 表示普通表：检查 tablesample 表达式（如果有） */
			if (walker(rte->tablesample, context))
				return true;
			break;
		case RTE_SUBQUERY:
			/*
			 * RTE 表示子查询：除非请求忽略 RT 子查询，否则调用 walker
			 * 子查询通常是一个 Query 节点。
			 */
			if (!(flags & QTW_IGNORE_RT_SUBQUERIES))
				if (walker(rte->subquery, context))
					return true;
			break;
		case RTE_JOIN:
			/*
			 * RTE 表示连接别名：除非忽略 join aliases，否则遍历 joinaliasvars，
			 * 它包含别名下的表达式列表（可能是 TargetEntry 等）。
			 */
			if (!(flags & QTW_IGNORE_JOINALIASES))
				if (walker(rte->joinaliasvars, context))
					return true;
			break;
		case RTE_FUNCTION:
			/* RTE 表示函数调用：遍历 functions 字段（函数表达式列表） */
			if (walker(rte->functions, context))
				return true;
			break;
		case RTE_TABLEFUNC:
			/* tablefunc 类型：遍历 tablefunc 结构（可能包含表达式） */
			if (walker(rte->tablefunc, context))
				return true;
			break;
		case RTE_VALUES:
			/* VALUES 列表：遍历 values_lists（表达式列表） */
			if (walker(rte->values_lists, context))
				return true;
			break;
		case RTE_CTE:
		case RTE_NAMEDTUPLESTORE:
		case RTE_RESULT:
			/* 这些类型没有表达式子项，直接跳过 */
			break;
	}

	/* 安全性或权限相关的谓词（可能是表达式列表），也需遍历 */
	if (walker(rte->securityQuals, context))
		return true;

	/* 在遍历 RTE 内容之后，按需再调用一次 walker */
	if (flags & QTW_EXAMINE_RTES_AFTER)
		if (walker(rte, context))
			return true;

	return false;
}


/*
 * expression_tree_mutator() 旨在支持那些创建表达式树修改副本的例程，
 * 在副本中某些节点可能被添加、删除或替换为新的子树。原始树（通常）
 * 不被修改。每个递归层负责返回其收到子树的副本（或适当修改的替代）。
 * 一个 mutator 例程应如下所示：
 *
 * Node * my_mutator (Node *node, my_struct *context)
 * {
 *		if (node == NULL)
 *			return NULL;
 *		// 检查那些需要特殊处理的节点，例如：
 *		if (IsA(node, Var))
 *		{
 *			... 创建并返回 Var 节点的修改副本
 *		}
 *		else if (IsA(node, ...))
 *		{
 *			... 对其他节点类型做特殊变换
 *		}
 *		// 对于未特别处理的节点类型，执行：
 *		return expression_tree_mutator(node, my_mutator, (void *) context);
 * }
 *
 * "context" 参数指向 mutator 所需的上下文结构 —— 也可用于通过 mutator
 * 返回额外的数据。expression_tree_mutator 不会修改此参数，但会将其传递给
 * 递归调用的 my_mutator。遍历从一个设置例程开始，它填充相应的上下文结构，
 * 用顶层节点调用 my_mutator，并执行任何必要的后处理。
 *
 * 每个递归层必须返回一个适当修改的 Node。如果调用 expression_tree_mutator()
 * 它将对给定节点做精确拷贝，但会调用 my_mutator() 来复制该节点的子节点。
 * 通过这种方式，my_mutator() 对复制过程拥有完全控制，但不必直接处理它不
 * 感兴趣的表达式子树。
 *
 * 与 expression_tree_walker 类似，expression_tree_mutator 所处理的节点类型
 * 包含在规划阶段通常出现在目标列表和谓词中的所有类型。
 *
 * expression_tree_mutator 会对 SubLink 节点常规地递归到 "testexpr" 子树
 * （属于外层计划）。它也会对 sublink 的子查询节点调用 mutator；但是当
 * expression_tree_mutator 自身被调用于一个 Query 节点时，它什么也不做并
 * 返回未修改的 Query 节点。这样一来，除非 mutator 在 Query 节点处有特殊处理，
 * 否则子查询不会被访问或修改；新的 SubLink 节点会链接到原始的子查询。
 * 想要深入子查询的 mutator 通常会在识别到 Query 节点时调用 query_tree_mutator。
 *
 * expression_tree_mutator 会对 SubPlan 节点递归变换 "testexpr" 和 "args" 列表
 *（它们属于外层计划），但会简单地复制对内部 Plan 的引用，因为这通常是
 *表达式树变换所期望的。若 mutator 想修改子计划，应在识别 SubPlan 表达式
 *节点时自行做适当处理。
 */

Node *
expression_tree_mutator(Node *node,
						Node *(*mutator) (),
						void *context)
{
	/*
	 * mutator 已经决定不修改当前节点，但我们必须对任何子节点调用 mutator。
	 */

#define FLATCOPY(newnode, node, nodetype)  \
	( (newnode) = (nodetype *) palloc(sizeof(nodetype)), \
	  memcpy((newnode), (node), sizeof(nodetype)) )

#define CHECKFLATCOPY(newnode, node, nodetype)	\
	( AssertMacro(IsA((node), nodetype)), \
	  (newnode) = (nodetype *) palloc(sizeof(nodetype)), \
	  memcpy((newnode), (node), sizeof(nodetype)) )

#define MUTATE(newfield, oldfield, fieldtype)  \
		( (newfield) = (fieldtype) mutator((Node *) (oldfield), context) )

	if (node == NULL)
		return NULL;

	/* 防止由于表达式过于复杂导致的栈溢出 */
	check_stack_depth();

	switch (nodeTag(node))
	{
			/*
			 * 无表达式子节点的原语节点类型。Var 和 Const 出现频繁，
			 * 值得特殊处理，其它类型使用 copyObject 即可。
			 */
		case T_Var:
			{
				Var		   *var = (Var *) node;
				Var		   *newnode;

				FLATCOPY(newnode, var, Var);
				return (Node *) newnode;
			}
			break;
		case T_Const:
			{
				Const	   *oldnode = (Const *) node;
				Const	   *newnode;

				FLATCOPY(newnode, oldnode, Const);
				/* XXX 我们不对 Datum 做深拷贝；是否需要？ */
				return (Node *) newnode;
			}
			break;
		case T_Param:
		case T_CaseTestExpr:
		case T_SQLValueFunction:
		case T_CoerceToDomainValue:
		case T_SetToDefault:
		case T_CurrentOfExpr:
		case T_NextValueExpr:
		case T_RangeTblRef:
		case T_SortGroupClause:
			return (Node *) copyObject(node);
		case T_WithCheckOption:
			{
				WithCheckOption *wco = (WithCheckOption *) node;
				WithCheckOption *newnode;

				FLATCOPY(newnode, wco, WithCheckOption);
				MUTATE(newnode->qual, wco->qual, Node *);
				return (Node *) newnode;
			}
		case T_Aggref:
			{
				Aggref	   *aggref = (Aggref *) node;
				Aggref	   *newnode;

				FLATCOPY(newnode, aggref, Aggref);
				/* 假定变换不会改变参数类型 */
				newnode->aggargtypes = list_copy(aggref->aggargtypes);
				MUTATE(newnode->aggdirectargs, aggref->aggdirectargs, List *);
				MUTATE(newnode->args, aggref->args, List *);
				MUTATE(newnode->aggorder, aggref->aggorder, List *);
				MUTATE(newnode->aggdistinct, aggref->aggdistinct, List *);
				MUTATE(newnode->aggfilter, aggref->aggfilter, Expr *);
				return (Node *) newnode;
			}
			break;
		case T_GroupingFunc:
			{
				GroupingFunc *grouping = (GroupingFunc *) node;
				GroupingFunc *newnode;

				FLATCOPY(newnode, grouping, GroupingFunc);
				MUTATE(newnode->args, grouping->args, List *);

				/*
				 * 这里假定变换参数不会改变语义，即参数不会被变换成与
				 * GROUP BY 子句中原来的表达式语义不同的内容。
				 *
				 * 如果某个 mutator 想这么做，它必须自行处理 refs 和 cols 列表。
				 */
				newnode->refs = list_copy(grouping->refs);
				newnode->cols = list_copy(grouping->cols);

				return (Node *) newnode;
			}
			break;
		case T_WindowFunc:
			{
				WindowFunc *wfunc = (WindowFunc *) node;
				WindowFunc *newnode;

				FLATCOPY(newnode, wfunc, WindowFunc);
				MUTATE(newnode->args, wfunc->args, List *);
				MUTATE(newnode->aggfilter, wfunc->aggfilter, Expr *);
				return (Node *) newnode;
			}
			break;
		case T_SubscriptingRef:
			{
				SubscriptingRef *sbsref = (SubscriptingRef *) node;
				SubscriptingRef *newnode;

				FLATCOPY(newnode, sbsref, SubscriptingRef);
				MUTATE(newnode->refupperindexpr, sbsref->refupperindexpr,
					   List *);
				MUTATE(newnode->reflowerindexpr, sbsref->reflowerindexpr,
					   List *);
				MUTATE(newnode->refexpr, sbsref->refexpr,
					   Expr *);
				MUTATE(newnode->refassgnexpr, sbsref->refassgnexpr,
					   Expr *);

				return (Node *) newnode;
			}
			break;
		case T_FuncExpr:
			{
				FuncExpr   *expr = (FuncExpr *) node;
				FuncExpr   *newnode;

				FLATCOPY(newnode, expr, FuncExpr);
				MUTATE(newnode->args, expr->args, List *);
				return (Node *) newnode;
			}
			break;
		case T_NamedArgExpr:
			{
				NamedArgExpr *nexpr = (NamedArgExpr *) node;
				NamedArgExpr *newnode;

				FLATCOPY(newnode, nexpr, NamedArgExpr);
				MUTATE(newnode->arg, nexpr->arg, Expr *);
				return (Node *) newnode;
			}
			break;
		case T_OpExpr:
			{
				OpExpr	   *expr = (OpExpr *) node;
				OpExpr	   *newnode;

				FLATCOPY(newnode, expr, OpExpr);
				MUTATE(newnode->args, expr->args, List *);
				return (Node *) newnode;
			}
			break;
		case T_DistinctExpr:
			{
				DistinctExpr *expr = (DistinctExpr *) node;
				DistinctExpr *newnode;

				FLATCOPY(newnode, expr, DistinctExpr);
				MUTATE(newnode->args, expr->args, List *);
				return (Node *) newnode;
			}
			break;
		case T_NullIfExpr:
			{
				NullIfExpr *expr = (NullIfExpr *) node;
				NullIfExpr *newnode;

				FLATCOPY(newnode, expr, NullIfExpr);
				MUTATE(newnode->args, expr->args, List *);
				return (Node *) newnode;
			}
			break;
		case T_ScalarArrayOpExpr:
			{
				ScalarArrayOpExpr *expr = (ScalarArrayOpExpr *) node;
				ScalarArrayOpExpr *newnode;

				FLATCOPY(newnode, expr, ScalarArrayOpExpr);
				MUTATE(newnode->args, expr->args, List *);
				return (Node *) newnode;
			}
			break;
		case T_BoolExpr:
			{
				BoolExpr   *expr = (BoolExpr *) node;
				BoolExpr   *newnode;

				FLATCOPY(newnode, expr, BoolExpr);
				MUTATE(newnode->args, expr->args, List *);
				return (Node *) newnode;
			}
			break;
		case T_SubLink:
			{
				SubLink    *sublink = (SubLink *) node;
				SubLink    *newnode;

				FLATCOPY(newnode, sublink, SubLink);
				MUTATE(newnode->testexpr, sublink->testexpr, Node *);

				/*
				 * 也对 sublink 的 Query 节点调用 mutator，使其可以选择
				 * 递归进入子查询。
				 */
				MUTATE(newnode->subselect, sublink->subselect, Node *);
				return (Node *) newnode;
			}
			break;
		case T_SubPlan:
			{
				SubPlan    *subplan = (SubPlan *) node;
				SubPlan    *newnode;

				FLATCOPY(newnode, subplan, SubPlan);
				/* 转换 testexpr */
				MUTATE(newnode->testexpr, subplan->testexpr, Node *);
				/* 转换 args 列表（传递给子计划的参数） */
				MUTATE(newnode->args, subplan->args, List *);
				/* 但不变换被引用的子 Plan 本身 */
				return (Node *) newnode;
			}
			break;
		case T_AlternativeSubPlan:
			{
				AlternativeSubPlan *asplan = (AlternativeSubPlan *) node;
				AlternativeSubPlan *newnode;

				FLATCOPY(newnode, asplan, AlternativeSubPlan);
				MUTATE(newnode->subplans, asplan->subplans, List *);
				return (Node *) newnode;
			}
			break;
		case T_FieldSelect:
			{
				FieldSelect *fselect = (FieldSelect *) node;
				FieldSelect *newnode;

				FLATCOPY(newnode, fselect, FieldSelect);
				MUTATE(newnode->arg, fselect->arg, Expr *);
				return (Node *) newnode;
			}
			break;
		case T_FieldStore:
			{
				FieldStore *fstore = (FieldStore *) node;
				FieldStore *newnode;

				FLATCOPY(newnode, fstore, FieldStore);
				MUTATE(newnode->arg, fstore->arg, Expr *);
				MUTATE(newnode->newvals, fstore->newvals, List *);
				newnode->fieldnums = list_copy(fstore->fieldnums);
				return (Node *) newnode;
			}
			break;
		case T_RelabelType:
			{
				RelabelType *relabel = (RelabelType *) node;
				RelabelType *newnode;

				FLATCOPY(newnode, relabel, RelabelType);
				MUTATE(newnode->arg, relabel->arg, Expr *);
				return (Node *) newnode;
			}
			break;
		case T_CoerceViaIO:
			{
				CoerceViaIO *iocoerce = (CoerceViaIO *) node;
				CoerceViaIO *newnode;

				FLATCOPY(newnode, iocoerce, CoerceViaIO);
				MUTATE(newnode->arg, iocoerce->arg, Expr *);
				return (Node *) newnode;
			}
			break;
		case T_ArrayCoerceExpr:
			{
				ArrayCoerceExpr *acoerce = (ArrayCoerceExpr *) node;
				ArrayCoerceExpr *newnode;

				FLATCOPY(newnode, acoerce, ArrayCoerceExpr);
				MUTATE(newnode->arg, acoerce->arg, Expr *);
				MUTATE(newnode->elemexpr, acoerce->elemexpr, Expr *);
				return (Node *) newnode;
			}
			break;
		case T_ConvertRowtypeExpr:
			{
				ConvertRowtypeExpr *convexpr = (ConvertRowtypeExpr *) node;
				ConvertRowtypeExpr *newnode;

				FLATCOPY(newnode, convexpr, ConvertRowtypeExpr);
				MUTATE(newnode->arg, convexpr->arg, Expr *);
				return (Node *) newnode;
			}
			break;
		case T_CollateExpr:
			{
				CollateExpr *collate = (CollateExpr *) node;
				CollateExpr *newnode;

				FLATCOPY(newnode, collate, CollateExpr);
				MUTATE(newnode->arg, collate->arg, Expr *);
				return (Node *) newnode;
			}
			break;
		case T_CaseExpr:
			{
				CaseExpr   *caseexpr = (CaseExpr *) node;
				CaseExpr   *newnode;

				FLATCOPY(newnode, caseexpr, CaseExpr);
				MUTATE(newnode->arg, caseexpr->arg, Expr *);
				MUTATE(newnode->args, caseexpr->args, List *);
				MUTATE(newnode->defresult, caseexpr->defresult, Expr *);
				return (Node *) newnode;
			}
			break;
		case T_CaseWhen:
			{
				CaseWhen   *casewhen = (CaseWhen *) node;
				CaseWhen   *newnode;

				FLATCOPY(newnode, casewhen, CaseWhen);
				MUTATE(newnode->expr, casewhen->expr, Expr *);
				MUTATE(newnode->result, casewhen->result, Expr *);
				return (Node *) newnode;
			}
			break;
		case T_ArrayExpr:
			{
				ArrayExpr  *arrayexpr = (ArrayExpr *) node;
				ArrayExpr  *newnode;

				FLATCOPY(newnode, arrayexpr, ArrayExpr);
				MUTATE(newnode->elements, arrayexpr->elements, List *);
				return (Node *) newnode;
			}
			break;
		case T_RowExpr:
			{
				RowExpr    *rowexpr = (RowExpr *) node;
				RowExpr    *newnode;

				FLATCOPY(newnode, rowexpr, RowExpr);
				MUTATE(newnode->args, rowexpr->args, List *);
				/* 假定 colnames 不需要复制 */
				return (Node *) newnode;
			}
			break;
		case T_RowCompareExpr:
			{
				RowCompareExpr *rcexpr = (RowCompareExpr *) node;
				RowCompareExpr *newnode;

				FLATCOPY(newnode, rcexpr, RowCompareExpr);
				MUTATE(newnode->largs, rcexpr->largs, List *);
				MUTATE(newnode->rargs, rcexpr->rargs, List *);
				return (Node *) newnode;
			}
			break;
		case T_CoalesceExpr:
			{
				CoalesceExpr *coalesceexpr = (CoalesceExpr *) node;
				CoalesceExpr *newnode;

				FLATCOPY(newnode, coalesceexpr, CoalesceExpr);
				MUTATE(newnode->args, coalesceexpr->args, List *);
				return (Node *) newnode;
			}
			break;
		case T_MinMaxExpr:
			{
				MinMaxExpr *minmaxexpr = (MinMaxExpr *) node;
				MinMaxExpr *newnode;

				FLATCOPY(newnode, minmaxexpr, MinMaxExpr);
				MUTATE(newnode->args, minmaxexpr->args, List *);
				return (Node *) newnode;
			}
			break;
		case T_XmlExpr:
			{
				XmlExpr    *xexpr = (XmlExpr *) node;
				XmlExpr    *newnode;

				FLATCOPY(newnode, xexpr, XmlExpr);
				MUTATE(newnode->named_args, xexpr->named_args, List *);
				/* 假定 mutator 不在意 arg_names */
				MUTATE(newnode->args, xexpr->args, List *);
				return (Node *) newnode;
			}
			break;
		case T_NullTest:
			{
				NullTest   *ntest = (NullTest *) node;
				NullTest   *newnode;

				FLATCOPY(newnode, ntest, NullTest);
				MUTATE(newnode->arg, ntest->arg, Expr *);
				return (Node *) newnode;
			}
			break;
		case T_BooleanTest:
			{
				BooleanTest *btest = (BooleanTest *) node;
				BooleanTest *newnode;

				FLATCOPY(newnode, btest, BooleanTest);
				MUTATE(newnode->arg, btest->arg, Expr *);
				return (Node *) newnode;
			}
			break;
		case T_CoerceToDomain:
			{
				CoerceToDomain *ctest = (CoerceToDomain *) node;
				CoerceToDomain *newnode;

				FLATCOPY(newnode, ctest, CoerceToDomain);
				MUTATE(newnode->arg, ctest->arg, Expr *);
				return (Node *) newnode;
			}
			break;
		case T_TargetEntry:
			{
				TargetEntry *targetentry = (TargetEntry *) node;
				TargetEntry *newnode;

				FLATCOPY(newnode, targetentry, TargetEntry);
				MUTATE(newnode->expr, targetentry->expr, Expr *);
				return (Node *) newnode;
			}
			break;
		case T_Query:
			/* 按上文讨论，对子 Query 什么也不做 */
			return node;
		case T_WindowClause:
			{
				WindowClause *wc = (WindowClause *) node;
				WindowClause *newnode;

				FLATCOPY(newnode, wc, WindowClause);
				MUTATE(newnode->partitionClause, wc->partitionClause, List *);
				MUTATE(newnode->orderClause, wc->orderClause, List *);
				MUTATE(newnode->startOffset, wc->startOffset, Node *);
				MUTATE(newnode->endOffset, wc->endOffset, Node *);
				return (Node *) newnode;
			}
			break;
		case T_CommonTableExpr:
			{
				CommonTableExpr *cte = (CommonTableExpr *) node;
				CommonTableExpr *newnode;

				FLATCOPY(newnode, cte, CommonTableExpr);

				/*
				 * 也对 CTE 的 Query 节点调用 mutator，使其可以选择递归进入子查询。
				 */
				MUTATE(newnode->ctequery, cte->ctequery, Node *);
				return (Node *) newnode;
			}
			break;
		case T_List:
			{
				/*
				 * 假定 mutator 不关心 List 节点本身，因此只对每个元素调用它。
				 * 注意：若列表的元素是整数，此处会出错！
				 */
				List	   *resultlist;
				ListCell   *temp;

				resultlist = NIL;
				foreach(temp, (List *) node)
				{
					resultlist = lappend(resultlist,
										 mutator((Node *) lfirst(temp),
												 context));
				}
				return (Node *) resultlist;
			}
			break;
		case T_FromExpr:
			{
				FromExpr   *from = (FromExpr *) node;
				FromExpr   *newnode;

				FLATCOPY(newnode, from, FromExpr);
				MUTATE(newnode->fromlist, from->fromlist, List *);
				MUTATE(newnode->quals, from->quals, Node *);
				return (Node *) newnode;
			}
			break;
		case T_OnConflictExpr:
			{
				OnConflictExpr *oc = (OnConflictExpr *) node;
				OnConflictExpr *newnode;

				FLATCOPY(newnode, oc, OnConflictExpr);
				MUTATE(newnode->arbiterElems, oc->arbiterElems, List *);
				MUTATE(newnode->arbiterWhere, oc->arbiterWhere, Node *);
				MUTATE(newnode->onConflictSet, oc->onConflictSet, List *);
				MUTATE(newnode->onConflictWhere, oc->onConflictWhere, Node *);
				MUTATE(newnode->exclRelTlist, oc->exclRelTlist, List *);

				return (Node *) newnode;
			}
			break;
		case T_PartitionPruneStepOp:
			{
				PartitionPruneStepOp *opstep = (PartitionPruneStepOp *) node;
				PartitionPruneStepOp *newnode;

				FLATCOPY(newnode, opstep, PartitionPruneStepOp);
				MUTATE(newnode->exprs, opstep->exprs, List *);

				return (Node *) newnode;
			}
			break;
		case T_PartitionPruneStepCombine:
			/* 无表达式子节点 */
			return (Node *) copyObject(node);
		case T_JoinExpr:
			{
				JoinExpr   *join = (JoinExpr *) node;
				JoinExpr   *newnode;

				FLATCOPY(newnode, join, JoinExpr);
				MUTATE(newnode->larg, join->larg, Node *);
				MUTATE(newnode->rarg, join->rarg, Node *);
				MUTATE(newnode->quals, join->quals, Node *);
				/* 默认不变换 alias 或 using */
				return (Node *) newnode;
			}
			break;
		case T_SetOperationStmt:
			{
				SetOperationStmt *setop = (SetOperationStmt *) node;
				SetOperationStmt *newnode;

				FLATCOPY(newnode, setop, SetOperationStmt);
				MUTATE(newnode->larg, setop->larg, Node *);
				MUTATE(newnode->rarg, setop->rarg, Node *);
				/* 默认不变换 groupClauses */
				return (Node *) newnode;
			}
			break;
		case T_IndexClause:
			{
				IndexClause *iclause = (IndexClause *) node;
				IndexClause *newnode;

				FLATCOPY(newnode, iclause, IndexClause);
				MUTATE(newnode->rinfo, iclause->rinfo, RestrictInfo *);
				MUTATE(newnode->indexquals, iclause->indexquals, List *);
				return (Node *) newnode;
			}
			break;
		case T_PlaceHolderVar:
			{
				PlaceHolderVar *phv = (PlaceHolderVar *) node;
				PlaceHolderVar *newnode;

				FLATCOPY(newnode, phv, PlaceHolderVar);
				MUTATE(newnode->phexpr, phv->phexpr, Expr *);
				/* 假定不需要复制 relids bitmapset */
				return (Node *) newnode;
			}
			break;
		case T_InferenceElem:
			{
				InferenceElem *inferenceelemdexpr = (InferenceElem *) node;
				InferenceElem *newnode;

				FLATCOPY(newnode, inferenceelemdexpr, InferenceElem);
				MUTATE(newnode->expr, newnode->expr, Node *);
				return (Node *) newnode;
			}
			break;
		case T_AppendRelInfo:
			{
				AppendRelInfo *appinfo = (AppendRelInfo *) node;
				AppendRelInfo *newnode;

				FLATCOPY(newnode, appinfo, AppendRelInfo);
				MUTATE(newnode->translated_vars, appinfo->translated_vars, List *);
				return (Node *) newnode;
			}
			break;
		case T_PlaceHolderInfo:
			{
				PlaceHolderInfo *phinfo = (PlaceHolderInfo *) node;
				PlaceHolderInfo *newnode;

				FLATCOPY(newnode, phinfo, PlaceHolderInfo);
				MUTATE(newnode->ph_var, phinfo->ph_var, PlaceHolderVar *);
				/* 假定不需要复制 relids bitmapsets */
				return (Node *) newnode;
			}
			break;
		case T_RangeTblFunction:
			{
				RangeTblFunction *rtfunc = (RangeTblFunction *) node;
				RangeTblFunction *newnode;

				FLATCOPY(newnode, rtfunc, RangeTblFunction);
				MUTATE(newnode->funcexpr, rtfunc->funcexpr, Node *);
				/* 假定不需要复制 coldef 信息列表 */
				return (Node *) newnode;
			}
			break;
		case T_TableSampleClause:
			{
				TableSampleClause *tsc = (TableSampleClause *) node;
				TableSampleClause *newnode;

				FLATCOPY(newnode, tsc, TableSampleClause);
				MUTATE(newnode->args, tsc->args, List *);
				MUTATE(newnode->repeatable, tsc->repeatable, Expr *);
				return (Node *) newnode;
			}
			break;
		case T_TableFunc:
			{
				TableFunc  *tf = (TableFunc *) node;
				TableFunc  *newnode;

				FLATCOPY(newnode, tf, TableFunc);
				MUTATE(newnode->ns_uris, tf->ns_uris, List *);
				MUTATE(newnode->docexpr, tf->docexpr, Node *);
				MUTATE(newnode->rowexpr, tf->rowexpr, Node *);
				MUTATE(newnode->colexprs, tf->colexprs, List *);
				MUTATE(newnode->coldefexprs, tf->coldefexprs, List *);
				return (Node *) newnode;
			}
			break;
		default:
			elog(ERROR, "unrecognized node type: %d",
				 (int) nodeTag(node));
			break;
	}
	/* 理论上不会到达此处，但使编译器不报错 */
	return NULL;
}


/*
 * query_tree_mutator --- 启动对 Query 表达式的修改
 *
 * 这个例程的存在是为了减少需要知道 Query 中所有表达式子树位置的地方。
 * 注意它可用于在 Query 的顶层启动遍历，无论 mutator 是否打算深入子查询。
 * 它也可用于在 mutator 内部下降到子查询。
 *
 * 有些调用者希望在 Query 中抑制对某些项的变换，通常是因为他们需要对这些
 * 项进行特殊处理，或实际上不想深入子查询。通过 flags 参数的按位或来支持
 * 抑制对指定项的变换。可以按需添加更多的 flag 位。
 *
 * 通常会复制 Query 节点本身，但有些调用者希望在原地修改它；这时应在 flags
 * 中传入 QTW_DONT_COPY_QUERY。无论如何，所有被修改的子结构都会被安全地复制。
 */
Query *
query_tree_mutator(Query *query,
				   Node *(*mutator) (),
				   void *context,
				   int flags)
{
	Assert(query != NULL && IsA(query, Query));

	if (!(flags & QTW_DONT_COPY_QUERY))
	{
		Query	   *newquery;

		FLATCOPY(newquery, query, Query);
		query = newquery;
	}

	MUTATE(query->targetList, query->targetList, List *);
	MUTATE(query->withCheckOptions, query->withCheckOptions, List *);
	MUTATE(query->onConflict, query->onConflict, OnConflictExpr *);
	MUTATE(query->returningList, query->returningList, List *);
	MUTATE(query->jointree, query->jointree, FromExpr *);
	MUTATE(query->setOperations, query->setOperations, Node *);
	MUTATE(query->havingQual, query->havingQual, Node *);
	MUTATE(query->limitOffset, query->limitOffset, Node *);
	MUTATE(query->limitCount, query->limitCount, Node *);

	/*
	 * 大多数调用者对 SortGroupClause 节点不感兴趣，因为它们不包含实际的表达式。
	 * 但是这些节点包含 OID，某些 mutator 可能会对它们感兴趣。
	 */

	if ((flags & QTW_EXAMINE_SORTGROUP))
	{
		MUTATE(query->groupClause, query->groupClause, List *);
		MUTATE(query->windowClause, query->windowClause, List *);
		MUTATE(query->sortClause, query->sortClause, List *);
		MUTATE(query->distinctClause, query->distinctClause, List *);
	}
	else
	{
		/*
		 * 即便我们对 SortGroupClause 节点不感兴趣，也需要变换 WindowClause 节点下的表达式。
		 */
		List	   *resultlist;
		ListCell   *temp;

		resultlist = NIL;
		foreach(temp, query->windowClause)
		{
			WindowClause *wc = lfirst_node(WindowClause, temp);
			WindowClause *newnode;

			FLATCOPY(newnode, wc, WindowClause);
			MUTATE(newnode->startOffset, wc->startOffset, Node *);
			MUTATE(newnode->endOffset, wc->endOffset, Node *);

			resultlist = lappend(resultlist, (Node *) newnode);
		}
		query->windowClause = resultlist;
	}

	/*
	 * groupingSets 和 rowMarks 不会被变换：
	 *
	 * groupingSets 仅包含 ressortgroup 引用（整数），在没有对应的 groupClause 或 tlist
	 * 的情况下是没有意义的。因此，任何需要关心它们的 mutator 都应在其处理 Query 时自行处理。
	 *
	 * rowMarks 仅包含 rangetable 索引（和标志等），因此也应在 Query 级别由调用者处理。
	 */

	if (!(flags & QTW_IGNORE_CTE_SUBQUERIES))
		MUTATE(query->cteList, query->cteList, List *);
	else						/* 否则，将 CTE 列表作为原样拷贝 */
		query->cteList = copyObject(query->cteList);
	query->rtable = range_table_mutator(query->rtable,
										mutator, context, flags);
	return query;
}

/*
 * range_table_mutator 只是 query_tree_mutator 中处理查询的 rangetable 的一部分。
 * 将其单独拆出是因为在某些场景下单独使用也很有用。
 */
List *
range_table_mutator(List *rtable,
					Node *(*mutator) (),
					void *context,
					int flags)
{
	List	   *newrt = NIL;
	ListCell   *rt;

	foreach(rt, rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(rt);
		RangeTblEntry *newrte;

		FLATCOPY(newrte, rte, RangeTblEntry);
		switch (rte->rtekind)
		{
			case RTE_RELATION:
				MUTATE(newrte->tablesample, rte->tablesample,
					   TableSampleClause *);
				/* 我们不复制 eref、别名等；这样处理可以吗？ */
				break;
			case RTE_SUBQUERY:
				if (!(flags & QTW_IGNORE_RT_SUBQUERIES))
				{
					CHECKFLATCOPY(newrte->subquery, rte->subquery, Query);
					MUTATE(newrte->subquery, newrte->subquery, Query *);
				}
				else
				{
					/* 否则，按原样复制 RT 子查询 */
					newrte->subquery = copyObject(rte->subquery);
				}
				break;
			case RTE_JOIN:
				if (!(flags & QTW_IGNORE_JOINALIASES))
					MUTATE(newrte->joinaliasvars, rte->joinaliasvars, List *);
				else
				{
					/* 否则，按原样复制 join 别名 */
					newrte->joinaliasvars = copyObject(rte->joinaliasvars);
				}
				break;
			case RTE_FUNCTION:
				MUTATE(newrte->functions, rte->functions, List *);
				break;
			case RTE_TABLEFUNC:
				MUTATE(newrte->tablefunc, rte->tablefunc, TableFunc *);
				break;
			case RTE_VALUES:
				MUTATE(newrte->values_lists, rte->values_lists, List *);
				break;
			case RTE_CTE:
			case RTE_NAMEDTUPLESTORE:
			case RTE_RESULT:
				/* 无需处理 */
				break;
		}
		MUTATE(newrte->securityQuals, rte->securityQuals, List *);
		newrt = lappend(newrt, newrte);
	}
	return newrt;
}

/*
 * query_or_expression_tree_walker --- hybrid form
 *
 * This routine will invoke query_tree_walker if called on a Query node,
 * else will invoke the walker directly.  This is a useful way of starting
 * the recursion when the walker's normal change of state is not appropriate
 * for the outermost Query node.
 */
bool
query_or_expression_tree_walker(Node *node,
								bool (*walker) (),
								void *context,
								int flags)
{
	if (node && IsA(node, Query))
		return query_tree_walker((Query *) node,
								 walker,
								 context,
								 flags);
	else
		return walker(node, context);
}

/*
 * query_or_expression_tree_mutator --- hybrid form
 *
 * This routine will invoke query_tree_mutator if called on a Query node,
 * else will invoke the mutator directly.  This is a useful way of starting
 * the recursion when the mutator's normal change of state is not appropriate
 * for the outermost Query node.
 */
Node *
query_or_expression_tree_mutator(Node *node,
								 Node *(*mutator) (),
								 void *context,
								 int flags)
{
	if (node && IsA(node, Query))
		return (Node *) query_tree_mutator((Query *) node,
										   mutator,
										   context,
										   flags);
	else
		return mutator(node, context);
}


/*
 * raw_expression_tree_walker --- walk raw parse trees
 *
 * This has exactly the same API as expression_tree_walker, but instead of
 * walking post-analysis parse trees, it knows how to walk the node types
 * found in raw grammar output.  (There is not currently any need for a
 * combined walker, so we keep them separate in the name of efficiency.)
 * Unlike expression_tree_walker, there is no special rule about query
 * boundaries: we descend to everything that's possibly interesting.
 *
 * Currently, the node type coverage here extends only to DML statements
 * (SELECT/INSERT/UPDATE/DELETE) and nodes that can appear in them, because
 * this is used mainly during analysis of CTEs, and only DML statements can
 * appear in CTEs.
 */
bool
raw_expression_tree_walker(Node *node,
						   bool (*walker) (),
						   void *context)
{
	ListCell   *temp;

	/*
	 * The walker has already visited the current node, and so we need only
	 * recurse into any sub-nodes it has.
	 */
	if (node == NULL)
		return false;

	/* Guard against stack overflow due to overly complex expressions */
	check_stack_depth();

	switch (nodeTag(node))
	{
		case T_SetToDefault:
		case T_CurrentOfExpr:
		case T_SQLValueFunction:
		case T_Integer:
		case T_Float:
		case T_String:
		case T_BitString:
		case T_Null:
		case T_ParamRef:
		case T_A_Const:
		case T_A_Star:
			/* primitive node types with no subnodes */
			break;
		case T_Alias:
			/* we assume the colnames list isn't interesting */
			break;
		case T_RangeVar:
			return walker(((RangeVar *) node)->alias, context);
		case T_GroupingFunc:
			return walker(((GroupingFunc *) node)->args, context);
		case T_SubLink:
			{
				SubLink    *sublink = (SubLink *) node;

				if (walker(sublink->testexpr, context))
					return true;
				/* we assume the operName is not interesting */
				if (walker(sublink->subselect, context))
					return true;
			}
			break;
		case T_CaseExpr:
			{
				CaseExpr   *caseexpr = (CaseExpr *) node;

				if (walker(caseexpr->arg, context))
					return true;
				/* we assume walker doesn't care about CaseWhens, either */
				foreach(temp, caseexpr->args)
				{
					CaseWhen   *when = lfirst_node(CaseWhen, temp);

					if (walker(when->expr, context))
						return true;
					if (walker(when->result, context))
						return true;
				}
				if (walker(caseexpr->defresult, context))
					return true;
			}
			break;
		case T_RowExpr:
			/* Assume colnames isn't interesting */
			return walker(((RowExpr *) node)->args, context);
		case T_CoalesceExpr:
			return walker(((CoalesceExpr *) node)->args, context);
		case T_MinMaxExpr:
			return walker(((MinMaxExpr *) node)->args, context);
		case T_XmlExpr:
			{
				XmlExpr    *xexpr = (XmlExpr *) node;

				if (walker(xexpr->named_args, context))
					return true;
				/* we assume walker doesn't care about arg_names */
				if (walker(xexpr->args, context))
					return true;
			}
			break;
		case T_NullTest:
			return walker(((NullTest *) node)->arg, context);
		case T_BooleanTest:
			return walker(((BooleanTest *) node)->arg, context);
		case T_JoinExpr:
			{
				JoinExpr   *join = (JoinExpr *) node;

				if (walker(join->larg, context))
					return true;
				if (walker(join->rarg, context))
					return true;
				if (walker(join->quals, context))
					return true;
				if (walker(join->alias, context))
					return true;
				/* using list is deemed uninteresting */
			}
			break;
		case T_IntoClause:
			{
				IntoClause *into = (IntoClause *) node;

				if (walker(into->rel, context))
					return true;
				/* colNames, options are deemed uninteresting */
				/* viewQuery should be null in raw parsetree, but check it */
				if (walker(into->viewQuery, context))
					return true;
			}
			break;
		case T_List:
			foreach(temp, (List *) node)
			{
				if (walker((Node *) lfirst(temp), context))
					return true;
			}
			break;
		case T_InsertStmt:
			{
				InsertStmt *stmt = (InsertStmt *) node;

				if (walker(stmt->relation, context))
					return true;
				if (walker(stmt->cols, context))
					return true;
				if (walker(stmt->selectStmt, context))
					return true;
				if (walker(stmt->onConflictClause, context))
					return true;
				if (walker(stmt->returningList, context))
					return true;
				if (walker(stmt->withClause, context))
					return true;
			}
			break;
		case T_DeleteStmt:
			{
				DeleteStmt *stmt = (DeleteStmt *) node;

				if (walker(stmt->relation, context))
					return true;
				if (walker(stmt->usingClause, context))
					return true;
				if (walker(stmt->whereClause, context))
					return true;
				if (walker(stmt->returningList, context))
					return true;
				if (walker(stmt->withClause, context))
					return true;
			}
			break;
		case T_UpdateStmt:
			{
				UpdateStmt *stmt = (UpdateStmt *) node;

				if (walker(stmt->relation, context))
					return true;
				if (walker(stmt->targetList, context))
					return true;
				if (walker(stmt->whereClause, context))
					return true;
				if (walker(stmt->fromClause, context))
					return true;
				if (walker(stmt->returningList, context))
					return true;
				if (walker(stmt->withClause, context))
					return true;
			}
			break;
		case T_SelectStmt:
			{
				SelectStmt *stmt = (SelectStmt *) node;

				if (walker(stmt->distinctClause, context))
					return true;
				if (walker(stmt->intoClause, context))
					return true;
				if (walker(stmt->targetList, context))
					return true;
				if (walker(stmt->fromClause, context))
					return true;
				if (walker(stmt->whereClause, context))
					return true;
				if (walker(stmt->groupClause, context))
					return true;
				if (walker(stmt->havingClause, context))
					return true;
				if (walker(stmt->windowClause, context))
					return true;
				if (walker(stmt->valuesLists, context))
					return true;
				if (walker(stmt->sortClause, context))
					return true;
				if (walker(stmt->limitOffset, context))
					return true;
				if (walker(stmt->limitCount, context))
					return true;
				if (walker(stmt->lockingClause, context))
					return true;
				if (walker(stmt->withClause, context))
					return true;
				if (walker(stmt->larg, context))
					return true;
				if (walker(stmt->rarg, context))
					return true;
			}
			break;
		case T_A_Expr:
			{
				A_Expr	   *expr = (A_Expr *) node;

				if (walker(expr->lexpr, context))
					return true;
				if (walker(expr->rexpr, context))
					return true;
				/* operator name is deemed uninteresting */
			}
			break;
		case T_BoolExpr:
			{
				BoolExpr   *expr = (BoolExpr *) node;

				if (walker(expr->args, context))
					return true;
			}
			break;
		case T_ColumnRef:
			/* we assume the fields contain nothing interesting */
			break;
		case T_FuncCall:
			{
				FuncCall   *fcall = (FuncCall *) node;

				if (walker(fcall->args, context))
					return true;
				if (walker(fcall->agg_order, context))
					return true;
				if (walker(fcall->agg_filter, context))
					return true;
				if (walker(fcall->over, context))
					return true;
				/* function name is deemed uninteresting */
			}
			break;
		case T_NamedArgExpr:
			return walker(((NamedArgExpr *) node)->arg, context);
		case T_A_Indices:
			{
				A_Indices  *indices = (A_Indices *) node;

				if (walker(indices->lidx, context))
					return true;
				if (walker(indices->uidx, context))
					return true;
			}
			break;
		case T_A_Indirection:
			{
				A_Indirection *indir = (A_Indirection *) node;

				if (walker(indir->arg, context))
					return true;
				if (walker(indir->indirection, context))
					return true;
			}
			break;
		case T_A_ArrayExpr:
			return walker(((A_ArrayExpr *) node)->elements, context);
		case T_ResTarget:
			{
				ResTarget  *rt = (ResTarget *) node;

				if (walker(rt->indirection, context))
					return true;
				if (walker(rt->val, context))
					return true;
			}
			break;
		case T_MultiAssignRef:
			return walker(((MultiAssignRef *) node)->source, context);
		case T_TypeCast:
			{
				TypeCast   *tc = (TypeCast *) node;

				if (walker(tc->arg, context))
					return true;
				if (walker(tc->typeName, context))
					return true;
			}
			break;
		case T_CollateClause:
			return walker(((CollateClause *) node)->arg, context);
		case T_SortBy:
			return walker(((SortBy *) node)->node, context);
		case T_WindowDef:
			{
				WindowDef  *wd = (WindowDef *) node;

				if (walker(wd->partitionClause, context))
					return true;
				if (walker(wd->orderClause, context))
					return true;
				if (walker(wd->startOffset, context))
					return true;
				if (walker(wd->endOffset, context))
					return true;
			}
			break;
		case T_RangeSubselect:
			{
				RangeSubselect *rs = (RangeSubselect *) node;

				if (walker(rs->subquery, context))
					return true;
				if (walker(rs->alias, context))
					return true;
			}
			break;
		case T_RangeFunction:
			{
				RangeFunction *rf = (RangeFunction *) node;

				if (walker(rf->functions, context))
					return true;
				if (walker(rf->alias, context))
					return true;
				if (walker(rf->coldeflist, context))
					return true;
			}
			break;
		case T_RangeTableSample:
			{
				RangeTableSample *rts = (RangeTableSample *) node;

				if (walker(rts->relation, context))
					return true;
				/* method name is deemed uninteresting */
				if (walker(rts->args, context))
					return true;
				if (walker(rts->repeatable, context))
					return true;
			}
			break;
		case T_RangeTableFunc:
			{
				RangeTableFunc *rtf = (RangeTableFunc *) node;

				if (walker(rtf->docexpr, context))
					return true;
				if (walker(rtf->rowexpr, context))
					return true;
				if (walker(rtf->namespaces, context))
					return true;
				if (walker(rtf->columns, context))
					return true;
				if (walker(rtf->alias, context))
					return true;
			}
			break;
		case T_RangeTableFuncCol:
			{
				RangeTableFuncCol *rtfc = (RangeTableFuncCol *) node;

				if (walker(rtfc->colexpr, context))
					return true;
				if (walker(rtfc->coldefexpr, context))
					return true;
			}
			break;
		case T_TypeName:
			{
				TypeName   *tn = (TypeName *) node;

				if (walker(tn->typmods, context))
					return true;
				if (walker(tn->arrayBounds, context))
					return true;
				/* type name itself is deemed uninteresting */
			}
			break;
		case T_ColumnDef:
			{
				ColumnDef  *coldef = (ColumnDef *) node;

				if (walker(coldef->typeName, context))
					return true;
				if (walker(coldef->raw_default, context))
					return true;
				if (walker(coldef->collClause, context))
					return true;
				/* for now, constraints are ignored */
			}
			break;
		case T_IndexElem:
			{
				IndexElem  *indelem = (IndexElem *) node;

				if (walker(indelem->expr, context))
					return true;
				/* collation and opclass names are deemed uninteresting */
			}
			break;
		case T_GroupingSet:
			return walker(((GroupingSet *) node)->content, context);
		case T_LockingClause:
			return walker(((LockingClause *) node)->lockedRels, context);
		case T_XmlSerialize:
			{
				XmlSerialize *xs = (XmlSerialize *) node;

				if (walker(xs->expr, context))
					return true;
				if (walker(xs->typeName, context))
					return true;
			}
			break;
		case T_WithClause:
			return walker(((WithClause *) node)->ctes, context);
		case T_InferClause:
			{
				InferClause *stmt = (InferClause *) node;

				if (walker(stmt->indexElems, context))
					return true;
				if (walker(stmt->whereClause, context))
					return true;
			}
			break;
		case T_OnConflictClause:
			{
				OnConflictClause *stmt = (OnConflictClause *) node;

				if (walker(stmt->infer, context))
					return true;
				if (walker(stmt->targetList, context))
					return true;
				if (walker(stmt->whereClause, context))
					return true;
			}
			break;
		case T_CommonTableExpr:
			return walker(((CommonTableExpr *) node)->ctequery, context);
		default:
			elog(ERROR, "unrecognized node type: %d",
				 (int) nodeTag(node));
			break;
	}
	return false;
}

/*
 * planstate_tree_walker --- walk plan state trees
 *
 * The walker has already visited the current node, and so we need only
 * recurse into any sub-nodes it has.
 */
bool
planstate_tree_walker(PlanState *planstate,
					  bool (*walker) (),
					  void *context)
{
	Plan	   *plan = planstate->plan;
	ListCell   *lc;

	/* Guard against stack overflow due to overly complex plan trees */
	check_stack_depth();

	/* initPlan-s */
	if (planstate_walk_subplans(planstate->initPlan, walker, context))
		return true;

	/* lefttree */
	if (outerPlanState(planstate))
	{
		if (walker(outerPlanState(planstate), context))
			return true;
	}

	/* righttree */
	if (innerPlanState(planstate))
	{
		if (walker(innerPlanState(planstate), context))
			return true;
	}

	/* special child plans */
	switch (nodeTag(plan))
	{
		case T_ModifyTable:
			if (planstate_walk_members(((ModifyTableState *) planstate)->mt_plans,
									   ((ModifyTableState *) planstate)->mt_nplans,
									   walker, context))
				return true;
			break;
		case T_Append:
			if (planstate_walk_members(((AppendState *) planstate)->appendplans,
									   ((AppendState *) planstate)->as_nplans,
									   walker, context))
				return true;
			break;
		case T_MergeAppend:
			if (planstate_walk_members(((MergeAppendState *) planstate)->mergeplans,
									   ((MergeAppendState *) planstate)->ms_nplans,
									   walker, context))
				return true;
			break;
		case T_BitmapAnd:
			if (planstate_walk_members(((BitmapAndState *) planstate)->bitmapplans,
									   ((BitmapAndState *) planstate)->nplans,
									   walker, context))
				return true;
			break;
		case T_BitmapOr:
			if (planstate_walk_members(((BitmapOrState *) planstate)->bitmapplans,
									   ((BitmapOrState *) planstate)->nplans,
									   walker, context))
				return true;
			break;
		case T_SubqueryScan:
			if (walker(((SubqueryScanState *) planstate)->subplan, context))
				return true;
			break;
		case T_CustomScan:
			foreach(lc, ((CustomScanState *) planstate)->custom_ps)
			{
				if (walker((PlanState *) lfirst(lc), context))
					return true;
			}
			break;
		default:
			break;
	}

	/* subPlan-s */
	if (planstate_walk_subplans(planstate->subPlan, walker, context))
		return true;

	return false;
}

/*
 * Walk a list of SubPlans (or initPlans, which also use SubPlan nodes).
 */
static bool
planstate_walk_subplans(List *plans,
						bool (*walker) (),
						void *context)
{
	ListCell   *lc;

	foreach(lc, plans)
	{
		SubPlanState *sps = lfirst_node(SubPlanState, lc);

		if (walker(sps->planstate, context))
			return true;
	}

	return false;
}

/*
 * Walk the constituent plans of a ModifyTable, Append, MergeAppend,
 * BitmapAnd, or BitmapOr node.
 */
static bool
planstate_walk_members(PlanState **planstates, int nplans,
					   bool (*walker) (), void *context)
{
	int			j;

	for (j = 0; j < nplans; j++)
	{
		if (walker(planstates[j], context))
			return true;
	}

	return false;
}
