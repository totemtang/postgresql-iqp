/*-------------------------------------------------------------------------
 *
 * nodeMergejoinInc.c
 *	  routines of incremental versions to support merge joins
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeMergejoinInc.c
 *
 *-------------------------------------------------------------------------
 */
/*
 * INTERFACE ROUTINES
 *		ExecMergeJoinInc		incremental mergejoin outer and inner relations.
 *		ExecInitMergeJoinInc	creates and initializes run time states
 */

#include "postgres.h"

#include "access/nbtree.h"
#include "executor/execdebug.h"
#include "executor/nodeMergejoin.h"
#include "miscadmin.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"

#include "executor/incmeta.h"


/*
 * States of the ExecMergeJoin state machine
 */
#define EXEC_MJ_INITIALIZE_OUTER		1
#define EXEC_MJ_INITIALIZE_INNER		2
#define EXEC_MJ_JOINTUPLES				3
#define EXEC_MJ_NEXTOUTER				4
#define EXEC_MJ_TESTOUTER				5
#define EXEC_MJ_NEXTINNER				6
#define EXEC_MJ_SKIP_TEST				7
#define EXEC_MJ_SKIPOUTER_ADVANCE		8
#define EXEC_MJ_SKIPINNER_ADVANCE		9
#define EXEC_MJ_ENDOUTER				10
#define EXEC_MJ_ENDINNER				11

/*
 * Runtime data for each mergejoin clause
 */
typedef struct MergeJoinClauseData
{
	/* Executable expression trees */
	ExprState  *lexpr;			/* left-hand (outer) input expression */
	ExprState  *rexpr;			/* right-hand (inner) input expression */

	/*
	 * If we have a current left or right input tuple, the values of the
	 * expressions are loaded into these fields:
	 */
	Datum		ldatum;			/* current left-hand value */
	Datum		rdatum;			/* current right-hand value */
	bool		lisnull;		/* and their isnull flags */
	bool		risnull;

	/*
	 * Everything we need to know to compare the left and right values is
	 * stored here.
	 */
	SortSupportData ssup;
}			MergeJoinClauseData;

/* Result type for MJEvalOuterValues and MJEvalInnerValues */
typedef enum
{
	MJEVAL_MATCHABLE,			/* normal, potentially matchable tuple */
	MJEVAL_NONMATCHABLE,		/* tuple cannot join because it has a null */
	MJEVAL_ENDOFJOIN			/* end of input (physical or effective) */
} MJEvalResult;


#define MarkInnerTuple(innerTupleSlot, mergestate) \
	ExecCopySlot((mergestate)->mj_MarkedTupleSlot, (innerTupleSlot))


/*
 * MJEvalOuterValues
 *
 * Compute the values of the mergejoined expressions for the current
 * outer tuple.  We also detect whether it's impossible for the current
 * outer tuple to match anything --- this is true if it yields a NULL
 * input, since we assume mergejoin operators are strict.  If the NULL
 * is in the first join column, and that column sorts nulls last, then
 * we can further conclude that no following tuple can match anything
 * either, since they must all have nulls in the first column.  However,
 * that case is only interesting if we're not in FillOuter mode, else
 * we have to visit all the tuples anyway.
 *
 * For the convenience of callers, we also make this routine responsible
 * for testing for end-of-input (null outer tuple), and returning
 * MJEVAL_ENDOFJOIN when that's seen.  This allows the same code to be used
 * for both real end-of-input and the effective end-of-input represented by
 * a first-column NULL.
 *
 * We evaluate the values in OuterEContext, which can be reset each
 * time we move to a new tuple.
 */
static MJEvalResult
MJEvalOuterValues(MergeJoinState *mergestate)
{
	ExprContext *econtext = mergestate->mj_OuterEContext;
	MJEvalResult result = MJEVAL_MATCHABLE;
	int			i;
	MemoryContext oldContext;

	/* Check for end of outer subplan */
	if (TupIsNull(mergestate->mj_OuterTupleSlot)) 
    {
        if (TupIsComplete(mergestate->mj_OuterTupleSlot)) 
            mergestate->isComplete = true;
		return MJEVAL_ENDOFJOIN;
    }

	ResetExprContext(econtext);

	oldContext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

	econtext->ecxt_outertuple = mergestate->mj_OuterTupleSlot;

	for (i = 0; i < mergestate->mj_NumClauses; i++)
	{
		MergeJoinClause clause = &mergestate->mj_Clauses[i];

		clause->ldatum = ExecEvalExpr(clause->lexpr, econtext,
									  &clause->lisnull);
		if (clause->lisnull)
		{
			/* match is impossible; can we end the join early? */
			if (i == 0 && !clause->ssup.ssup_nulls_first &&
				!mergestate->mj_FillOuter)
				result = MJEVAL_ENDOFJOIN;
			else if (result == MJEVAL_MATCHABLE)
				result = MJEVAL_NONMATCHABLE;
		}
	}

	MemoryContextSwitchTo(oldContext);

	return result;
}

/*
 * MJEvalInnerValues
 *
 * Same as above, but for the inner tuple.  Here, we have to be prepared
 * to load data from either the true current inner, or the marked inner,
 * so caller must tell us which slot to load from.
 */
static MJEvalResult
MJEvalInnerValues(MergeJoinState *mergestate, TupleTableSlot *innerslot)
{
	ExprContext *econtext = mergestate->mj_InnerEContext;
	MJEvalResult result = MJEVAL_MATCHABLE;
	int			i;
	MemoryContext oldContext;

	/* Check for end of inner subplan */
	if (TupIsNull(innerslot))
		return MJEVAL_ENDOFJOIN;

	ResetExprContext(econtext);

	oldContext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

	econtext->ecxt_innertuple = innerslot;

	for (i = 0; i < mergestate->mj_NumClauses; i++)
	{
		MergeJoinClause clause = &mergestate->mj_Clauses[i];

		clause->rdatum = ExecEvalExpr(clause->rexpr, econtext,
									  &clause->risnull);
		if (clause->risnull)
		{
			/* match is impossible; can we end the join early? */
			if (i == 0 && !clause->ssup.ssup_nulls_first &&
				!mergestate->mj_FillInner)
				result = MJEVAL_ENDOFJOIN;
			else if (result == MJEVAL_MATCHABLE)
				result = MJEVAL_NONMATCHABLE;
		}
	}

	MemoryContextSwitchTo(oldContext);

	return result;
}

/*
 * MJCompare
 *
 * Compare the mergejoinable values of the current two input tuples
 * and return 0 if they are equal (ie, the mergejoin equalities all
 * succeed), >0 if outer > inner, <0 if outer < inner.
 *
 * MJEvalOuterValues and MJEvalInnerValues must already have been called
 * for the current outer and inner tuples, respectively.
 */
static int
MJCompare(MergeJoinState *mergestate)
{
	int			result = 0;
	bool		nulleqnull = false;
	ExprContext *econtext = mergestate->js.ps.ps_ExprContext;
	int			i;
	MemoryContext oldContext;

	/*
	 * Call the comparison functions in short-lived context, in case they leak
	 * memory.
	 */
	ResetExprContext(econtext);

	oldContext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

	for (i = 0; i < mergestate->mj_NumClauses; i++)
	{
		MergeJoinClause clause = &mergestate->mj_Clauses[i];

		/*
		 * Special case for NULL-vs-NULL, else use standard comparison.
		 */
		if (clause->lisnull && clause->risnull)
		{
			nulleqnull = true;	/* NULL "=" NULL */
			continue;
		}

		result = ApplySortComparator(clause->ldatum, clause->lisnull,
									 clause->rdatum, clause->risnull,
									 &clause->ssup);

		if (result != 0)
			break;
	}

	/*
	 * If we had any NULL-vs-NULL inputs, we do not want to report that the
	 * tuples are equal.  Instead, if result is still 0, change it to +1. This
	 * will result in advancing the inner side of the join.
	 *
	 * Likewise, if there was a constant-false joinqual, do not report
	 * equality.  We have to check this as part of the mergequals, else the
	 * rescan logic will do the wrong thing.
	 */
	if (result == 0 &&
		(nulleqnull || mergestate->mj_ConstFalseJoin))
		result = 1;

	MemoryContextSwitchTo(oldContext);

	return result;
}


/*
 * Generate a fake join tuple with nulls for the inner tuple,
 * and return it if it passes the non-join quals.
 */
static TupleTableSlot *
MJFillOuter(MergeJoinState *node)
{
	ExprContext *econtext = node->js.ps.ps_ExprContext;
	ExprState  *otherqual = node->js.ps.qual;

	ResetExprContext(econtext);

	econtext->ecxt_outertuple = node->mj_OuterTupleSlot;
	econtext->ecxt_innertuple = node->mj_NullInnerTupleSlot;

	if (ExecQual(otherqual, econtext))
	{
		/*
		 * qualification succeeded.  now form the desired projection tuple and
		 * return the slot containing it.
		 */
		MJ_printf("ExecMergeJoin: returning outer fill tuple\n");

		return ExecProject(node->js.ps.ps_ProjInfo);
	}
	else
		InstrCountFiltered2(node, 1);

	return NULL;
}

/*
 * Generate a fake join tuple with nulls for the outer tuple,
 * and return it if it passes the non-join quals.
 */
static TupleTableSlot *
MJFillInner(MergeJoinState *node)
{
	ExprContext *econtext = node->js.ps.ps_ExprContext;
	ExprState  *otherqual = node->js.ps.qual;

	ResetExprContext(econtext);

	econtext->ecxt_outertuple = node->mj_NullOuterTupleSlot;
	econtext->ecxt_innertuple = node->mj_InnerTupleSlot;

	if (ExecQual(otherqual, econtext))
	{
		/*
		 * qualification succeeded.  now form the desired projection tuple and
		 * return the slot containing it.
		 */
		MJ_printf("ExecMergeJoin: returning inner fill tuple\n");

		return ExecProject(node->js.ps.ps_ProjInfo);
	}
	else
		InstrCountFiltered2(node, 1);

	return NULL;
}


/*
 * Check that a qual condition is constant true or constant false.
 * If it is constant false (or null), set *is_const_false to TRUE.
 *
 * Constant true would normally be represented by a NIL list, but we allow an
 * actual bool Const as well.  We do expect that the planner will have thrown
 * away any non-constant terms that have been ANDed with a constant false.
 */
static bool
check_constant_qual(List *qual, bool *is_const_false)
{
	ListCell   *lc;

	foreach(lc, qual)
	{
		Const	   *con = (Const *) lfirst(lc);

		if (!con || !IsA(con, Const))
			return false;
		if (con->constisnull || !DatumGetBool(con->constvalue))
			*is_const_false = true;
	}
	return true;
}


/* ----------------------------------------------------------------
 *		ExecMergeTupleDump
 *
 *		This function is called through the MJ_dump() macro
 *		when EXEC_MERGEJOINDEBUG is defined
 * ----------------------------------------------------------------
 */
#ifdef EXEC_MERGEJOINDEBUG

static void
ExecMergeTupleDumpOuter(MergeJoinState *mergestate)
{
	TupleTableSlot *outerSlot = mergestate->mj_OuterTupleSlot;

	printf("==== outer tuple ====\n");
	if (TupIsNull(outerSlot))
		printf("(nil)\n");
	else
		MJ_debugtup(outerSlot);
}

static void
ExecMergeTupleDumpInner(MergeJoinState *mergestate)
{
	TupleTableSlot *innerSlot = mergestate->mj_InnerTupleSlot;

	printf("==== inner tuple ====\n");
	if (TupIsNull(innerSlot))
		printf("(nil)\n");
	else
		MJ_debugtup(innerSlot);
}

static void
ExecMergeTupleDumpMarked(MergeJoinState *mergestate)
{
	TupleTableSlot *markedSlot = mergestate->mj_MarkedTupleSlot;

	printf("==== marked tuple ====\n");
	if (TupIsNull(markedSlot))
		printf("(nil)\n");
	else
		MJ_debugtup(markedSlot);
}

static void
ExecMergeTupleDump(MergeJoinState *mergestate)
{
	printf("******** ExecMergeTupleDump ********\n");

	ExecMergeTupleDumpOuter(mergestate);
	ExecMergeTupleDumpInner(mergestate);
	ExecMergeTupleDumpMarked(mergestate);

	printf("********\n");
}
#endif

/* ----------------------------------------------------------------
 *		ExecMergeJoin
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
ExecMergeJoin(PlanState *pstate)
{
	MergeJoinState *node = castNode(MergeJoinState, pstate);
	ExprState  *joinqual;
	ExprState  *otherqual;
	bool		qualResult;
	int			compareResult;
	PlanState  *innerPlan;
	TupleTableSlot *innerTupleSlot;
	PlanState  *outerPlan;
	TupleTableSlot *outerTupleSlot;
	ExprContext *econtext;
	bool		doFillOuter;
	bool		doFillInner;

	CHECK_FOR_INTERRUPTS();

	/*
	 * get information from node
	 */
	innerPlan = innerPlanState(node);
	outerPlan = outerPlanState(node);
	econtext = node->js.ps.ps_ExprContext;
	joinqual = node->js.joinqual;
	otherqual = node->js.ps.qual;
	doFillOuter = node->mj_FillOuter;
	doFillInner = node->mj_FillInner;

	/*
	 * Reset per-tuple memory context to free any expression evaluation
	 * storage allocated in the previous tuple cycle.
	 */
	ResetExprContext(econtext);

	/*
	 * ok, everything is setup.. let's go to work
	 */
	for (;;)
	{
		MJ_dump(node);

		/*
		 * get the current state of the join and do things accordingly.
		 */
		switch (node->mj_JoinState)
		{
				/*
				 * EXEC_MJ_INITIALIZE_OUTER means that this is the first time
				 * ExecMergeJoin() has been called and so we have to fetch the
				 * first matchable tuple for both outer and inner subplans. We
				 * do the outer side in INITIALIZE_OUTER state, then advance
				 * to INITIALIZE_INNER state for the inner subplan.
				 */
			case EXEC_MJ_INITIALIZE_OUTER:
				MJ_printf("ExecMergeJoin: EXEC_MJ_INITIALIZE_OUTER\n");

				outerTupleSlot = ExecProcNode(outerPlan);
				node->mj_OuterTupleSlot = outerTupleSlot;

				/* Compute join values and check for unmatchability */
				switch (MJEvalOuterValues(node))
				{
					case MJEVAL_MATCHABLE:
						/* OK to go get the first inner tuple */
						node->mj_JoinState = EXEC_MJ_INITIALIZE_INNER;
						break;
					case MJEVAL_NONMATCHABLE:
						/* Stay in same state to fetch next outer tuple */
						if (doFillOuter)
						{
							/*
							 * Generate a fake join tuple with nulls for the
							 * inner tuple, and return it if it passes the
							 * non-join quals.
							 */
							TupleTableSlot *result;

							result = MJFillOuter(node);
							if (result)
								return result;
						}
						break;
					case MJEVAL_ENDOFJOIN:
						/* No more outer tuples */
						MJ_printf("ExecMergeJoin: nothing in outer subplan\n");
						if (doFillInner)
						{
							/*
							 * Need to emit right-join tuples for remaining
							 * inner tuples. We set MatchedInner = true to
							 * force the ENDOUTER state to advance inner.
							 */
							node->mj_JoinState = EXEC_MJ_ENDOUTER;
							node->mj_MatchedInner = true;
							break;
						}
						/* Otherwise we're done. */
						return NULL;
				}
				break;

			case EXEC_MJ_INITIALIZE_INNER:
				MJ_printf("ExecMergeJoin: EXEC_MJ_INITIALIZE_INNER\n");

				innerTupleSlot = ExecProcNode(innerPlan);
				node->mj_InnerTupleSlot = innerTupleSlot;

				/* Compute join values and check for unmatchability */
				switch (MJEvalInnerValues(node, innerTupleSlot))
				{
					case MJEVAL_MATCHABLE:

						/*
						 * OK, we have the initial tuples.  Begin by skipping
						 * non-matching tuples.
						 */
						node->mj_JoinState = EXEC_MJ_SKIP_TEST;
						break;
					case MJEVAL_NONMATCHABLE:
						/* Mark before advancing, if wanted */
						if (node->mj_ExtraMarks)
							ExecMarkPos(innerPlan);
						/* Stay in same state to fetch next inner tuple */
						if (doFillInner)
						{
							/*
							 * Generate a fake join tuple with nulls for the
							 * outer tuple, and return it if it passes the
							 * non-join quals.
							 */
							TupleTableSlot *result;

							result = MJFillInner(node);
							if (result)
								return result;
						}
						break;
					case MJEVAL_ENDOFJOIN:
						/* No more inner tuples */
						MJ_printf("ExecMergeJoin: nothing in inner subplan\n");
						if (doFillOuter)
						{
							/*
							 * Need to emit left-join tuples for all outer
							 * tuples, including the one we just fetched.  We
							 * set MatchedOuter = false to force the ENDINNER
							 * state to emit first tuple before advancing
							 * outer.
							 */
							node->mj_JoinState = EXEC_MJ_ENDINNER;
							node->mj_MatchedOuter = false;
							break;
						}
						/* Otherwise we're done. */
						return NULL;
				}
				break;

				/*
				 * EXEC_MJ_JOINTUPLES means we have two tuples which satisfied
				 * the merge clause so we join them and then proceed to get
				 * the next inner tuple (EXEC_MJ_NEXTINNER).
				 */
			case EXEC_MJ_JOINTUPLES:
				MJ_printf("ExecMergeJoin: EXEC_MJ_JOINTUPLES\n");

				/*
				 * Set the next state machine state.  The right things will
				 * happen whether we return this join tuple or just fall
				 * through to continue the state machine execution.
				 */
				node->mj_JoinState = EXEC_MJ_NEXTINNER;

				/*
				 * Check the extra qual conditions to see if we actually want
				 * to return this join tuple.  If not, can proceed with merge.
				 * We must distinguish the additional joinquals (which must
				 * pass to consider the tuples "matched" for outer-join logic)
				 * from the otherquals (which must pass before we actually
				 * return the tuple).
				 *
				 * We don't bother with a ResetExprContext here, on the
				 * assumption that we just did one while checking the merge
				 * qual.  One per tuple should be sufficient.  We do have to
				 * set up the econtext links to the tuples for ExecQual to
				 * use.
				 */
				outerTupleSlot = node->mj_OuterTupleSlot;
				econtext->ecxt_outertuple = outerTupleSlot;
				innerTupleSlot = node->mj_InnerTupleSlot;
				econtext->ecxt_innertuple = innerTupleSlot;

				qualResult = (joinqual == NULL ||
							  ExecQual(joinqual, econtext));
				MJ_DEBUG_QUAL(joinqual, qualResult);

				if (qualResult)
				{
					node->mj_MatchedOuter = true;
					node->mj_MatchedInner = true;

					/* In an antijoin, we never return a matched tuple */
					if (node->js.jointype == JOIN_ANTI)
					{
						node->mj_JoinState = EXEC_MJ_NEXTOUTER;
						break;
					}

					/*
					 * If we only need to join to the first matching inner
					 * tuple, then consider returning this one, but after that
					 * continue with next outer tuple.
					 */
					if (node->js.single_match)
						node->mj_JoinState = EXEC_MJ_NEXTOUTER;

					qualResult = (otherqual == NULL ||
								  ExecQual(otherqual, econtext));
					MJ_DEBUG_QUAL(otherqual, qualResult);

					if (qualResult)
					{
						/*
						 * qualification succeeded.  now form the desired
						 * projection tuple and return the slot containing it.
						 */
						MJ_printf("ExecMergeJoin: returning tuple\n");

						return ExecProject(node->js.ps.ps_ProjInfo);
					}
					else
						InstrCountFiltered2(node, 1);
				}
				else
					InstrCountFiltered1(node, 1);
				break;

				/*
				 * EXEC_MJ_NEXTINNER means advance the inner scan to the next
				 * tuple. If the tuple is not nil, we then proceed to test it
				 * against the join qualification.
				 *
				 * Before advancing, we check to see if we must emit an
				 * outer-join fill tuple for this inner tuple.
				 */
			case EXEC_MJ_NEXTINNER:
				MJ_printf("ExecMergeJoin: EXEC_MJ_NEXTINNER\n");

				if (doFillInner && !node->mj_MatchedInner)
				{
					/*
					 * Generate a fake join tuple with nulls for the outer
					 * tuple, and return it if it passes the non-join quals.
					 */
					TupleTableSlot *result;

					node->mj_MatchedInner = true;	/* do it only once */

					result = MJFillInner(node);
					if (result)
						return result;
				}

				/*
				 * now we get the next inner tuple, if any.  If there's none,
				 * advance to next outer tuple (which may be able to join to
				 * previously marked tuples).
				 *
				 * NB: must NOT do "extraMarks" here, since we may need to
				 * return to previously marked tuples.
				 */
				innerTupleSlot = ExecProcNode(innerPlan);
				node->mj_InnerTupleSlot = innerTupleSlot;
				MJ_DEBUG_PROC_NODE(innerTupleSlot);
				node->mj_MatchedInner = false;

				/* Compute join values and check for unmatchability */
				switch (MJEvalInnerValues(node, innerTupleSlot))
				{
					case MJEVAL_MATCHABLE:

						/*
						 * Test the new inner tuple to see if it matches
						 * outer.
						 *
						 * If they do match, then we join them and move on to
						 * the next inner tuple (EXEC_MJ_JOINTUPLES).
						 *
						 * If they do not match then advance to next outer
						 * tuple.
						 */
						compareResult = MJCompare(node);
						MJ_DEBUG_COMPARE(compareResult);

						if (compareResult == 0)
							node->mj_JoinState = EXEC_MJ_JOINTUPLES;
						else
						{
							Assert(compareResult < 0);
							node->mj_JoinState = EXEC_MJ_NEXTOUTER;
						}
						break;
					case MJEVAL_NONMATCHABLE:

						/*
						 * It contains a NULL and hence can't match any outer
						 * tuple, so we can skip the comparison and assume the
						 * new tuple is greater than current outer.
						 */
						node->mj_JoinState = EXEC_MJ_NEXTOUTER;
						break;
					case MJEVAL_ENDOFJOIN:

						/*
						 * No more inner tuples.  However, this might be only
						 * effective and not physical end of inner plan, so
						 * force mj_InnerTupleSlot to null to make sure we
						 * don't fetch more inner tuples.  (We need this hack
						 * because we are not transiting to a state where the
						 * inner plan is assumed to be exhausted.)
						 */
						node->mj_InnerTupleSlot = NULL;
						node->mj_JoinState = EXEC_MJ_NEXTOUTER;
						break;
				}
				break;

				/*-------------------------------------------
				 * EXEC_MJ_NEXTOUTER means
				 *
				 *				outer inner
				 * outer tuple -  5		5  - marked tuple
				 *				  5		5
				 *				  6		6  - inner tuple
				 *				  7		7
				 *
				 * we know we just bumped into the
				 * first inner tuple > current outer tuple (or possibly
				 * the end of the inner stream)
				 * so get a new outer tuple and then
				 * proceed to test it against the marked tuple
				 * (EXEC_MJ_TESTOUTER)
				 *
				 * Before advancing, we check to see if we must emit an
				 * outer-join fill tuple for this outer tuple.
				 *------------------------------------------------
				 */
			case EXEC_MJ_NEXTOUTER:
				MJ_printf("ExecMergeJoin: EXEC_MJ_NEXTOUTER\n");

				if (doFillOuter && !node->mj_MatchedOuter)
				{
					/*
					 * Generate a fake join tuple with nulls for the inner
					 * tuple, and return it if it passes the non-join quals.
					 */
					TupleTableSlot *result;

					node->mj_MatchedOuter = true;	/* do it only once */

					result = MJFillOuter(node);
					if (result)
						return result;
				}

				/*
				 * now we get the next outer tuple, if any
				 */
				outerTupleSlot = ExecProcNode(outerPlan);
				node->mj_OuterTupleSlot = outerTupleSlot;
				MJ_DEBUG_PROC_NODE(outerTupleSlot);
				node->mj_MatchedOuter = false;

				/* Compute join values and check for unmatchability */
				switch (MJEvalOuterValues(node))
				{
					case MJEVAL_MATCHABLE:
						/* Go test the new tuple against the marked tuple */
						node->mj_JoinState = EXEC_MJ_TESTOUTER;
						break;
					case MJEVAL_NONMATCHABLE:
						/* Can't match, so fetch next outer tuple */
						node->mj_JoinState = EXEC_MJ_NEXTOUTER;
						break;
					case MJEVAL_ENDOFJOIN:
						/* No more outer tuples */
						MJ_printf("ExecMergeJoin: end of outer subplan\n");
						innerTupleSlot = node->mj_InnerTupleSlot;
						if (doFillInner && !TupIsNull(innerTupleSlot))
						{
							/*
							 * Need to emit right-join tuples for remaining
							 * inner tuples.
							 */
							node->mj_JoinState = EXEC_MJ_ENDOUTER;
							break;
						}
						/* Otherwise we're done. */
						return NULL;
				}
				break;

				/*--------------------------------------------------------
				 * EXEC_MJ_TESTOUTER If the new outer tuple and the marked
				 * tuple satisfy the merge clause then we know we have
				 * duplicates in the outer scan so we have to restore the
				 * inner scan to the marked tuple and proceed to join the
				 * new outer tuple with the inner tuples.
				 *
				 * This is the case when
				 *						  outer inner
				 *							4	  5  - marked tuple
				 *			 outer tuple -	5	  5
				 *		 new outer tuple -	5	  5
				 *							6	  8  - inner tuple
				 *							7	 12
				 *
				 *				new outer tuple == marked tuple
				 *
				 * If the outer tuple fails the test, then we are done
				 * with the marked tuples, and we have to look for a
				 * match to the current inner tuple.  So we will
				 * proceed to skip outer tuples until outer >= inner
				 * (EXEC_MJ_SKIP_TEST).
				 *
				 *		This is the case when
				 *
				 *						  outer inner
				 *							5	  5  - marked tuple
				 *			 outer tuple -	5	  5
				 *		 new outer tuple -	6	  8  - inner tuple
				 *							7	 12
				 *
				 *				new outer tuple > marked tuple
				 *
				 *---------------------------------------------------------
				 */
			case EXEC_MJ_TESTOUTER:
				MJ_printf("ExecMergeJoin: EXEC_MJ_TESTOUTER\n");

				/*
				 * Here we must compare the outer tuple with the marked inner
				 * tuple.  (We can ignore the result of MJEvalInnerValues,
				 * since the marked inner tuple is certainly matchable.)
				 */
				innerTupleSlot = node->mj_MarkedTupleSlot;
				(void) MJEvalInnerValues(node, innerTupleSlot);

				compareResult = MJCompare(node);
				MJ_DEBUG_COMPARE(compareResult);

				if (compareResult == 0)
				{
					/*
					 * the merge clause matched so now we restore the inner
					 * scan position to the first mark, and go join that tuple
					 * (and any following ones) to the new outer.
					 *
					 * If we were able to determine mark and restore are not
					 * needed, then we don't have to back up; the current
					 * inner is already the first possible match.
					 *
					 * NOTE: we do not need to worry about the MatchedInner
					 * state for the rescanned inner tuples.  We know all of
					 * them will match this new outer tuple and therefore
					 * won't be emitted as fill tuples.  This works *only*
					 * because we require the extra joinquals to be constant
					 * when doing a right or full join --- otherwise some of
					 * the rescanned tuples might fail the extra joinquals.
					 * This obviously won't happen for a constant-true extra
					 * joinqual, while the constant-false case is handled by
					 * forcing the merge clause to never match, so we never
					 * get here.
					 */
					if (!node->mj_SkipMarkRestore)
					{
						ExecRestrPos(innerPlan);

						/*
						 * ExecRestrPos probably should give us back a new
						 * Slot, but since it doesn't, use the marked slot.
						 * (The previously returned mj_InnerTupleSlot cannot
						 * be assumed to hold the required tuple.)
						 */
						node->mj_InnerTupleSlot = innerTupleSlot;
						/* we need not do MJEvalInnerValues again */
					}

					node->mj_JoinState = EXEC_MJ_JOINTUPLES;
				}
				else
				{
					/* ----------------
					 *	if the new outer tuple didn't match the marked inner
					 *	tuple then we have a case like:
					 *
					 *			 outer inner
					 *			   4	 4	- marked tuple
					 * new outer - 5	 4
					 *			   6	 5	- inner tuple
					 *			   7
					 *
					 *	which means that all subsequent outer tuples will be
					 *	larger than our marked inner tuples.  So we need not
					 *	revisit any of the marked tuples but can proceed to
					 *	look for a match to the current inner.  If there's
					 *	no more inners, no more matches are possible.
					 * ----------------
					 */
					Assert(compareResult > 0);
					innerTupleSlot = node->mj_InnerTupleSlot;

					/* reload comparison data for current inner */
					switch (MJEvalInnerValues(node, innerTupleSlot))
					{
						case MJEVAL_MATCHABLE:
							/* proceed to compare it to the current outer */
							node->mj_JoinState = EXEC_MJ_SKIP_TEST;
							break;
						case MJEVAL_NONMATCHABLE:

							/*
							 * current inner can't possibly match any outer;
							 * better to advance the inner scan than the
							 * outer.
							 */
							node->mj_JoinState = EXEC_MJ_SKIPINNER_ADVANCE;
							break;
						case MJEVAL_ENDOFJOIN:
							/* No more inner tuples */
							if (doFillOuter)
							{
								/*
								 * Need to emit left-join tuples for remaining
								 * outer tuples.
								 */
								node->mj_JoinState = EXEC_MJ_ENDINNER;
								break;
							}
							/* Otherwise we're done. */
							return NULL;
					}
				}
				break;

				/*----------------------------------------------------------
				 * EXEC_MJ_SKIP means compare tuples and if they do not
				 * match, skip whichever is lesser.
				 *
				 * For example:
				 *
				 *				outer inner
				 *				  5		5
				 *				  5		5
				 * outer tuple -  6		8  - inner tuple
				 *				  7    12
				 *				  8    14
				 *
				 * we have to advance the outer scan
				 * until we find the outer 8.
				 *
				 * On the other hand:
				 *
				 *				outer inner
				 *				  5		5
				 *				  5		5
				 * outer tuple - 12		8  - inner tuple
				 *				 14    10
				 *				 17    12
				 *
				 * we have to advance the inner scan
				 * until we find the inner 12.
				 *----------------------------------------------------------
				 */
			case EXEC_MJ_SKIP_TEST:
				MJ_printf("ExecMergeJoin: EXEC_MJ_SKIP_TEST\n");

				/*
				 * before we advance, make sure the current tuples do not
				 * satisfy the mergeclauses.  If they do, then we update the
				 * marked tuple position and go join them.
				 */
				compareResult = MJCompare(node);
				MJ_DEBUG_COMPARE(compareResult);

				if (compareResult == 0)
				{
					if (!node->mj_SkipMarkRestore)
						ExecMarkPos(innerPlan);

					MarkInnerTuple(node->mj_InnerTupleSlot, node);

					node->mj_JoinState = EXEC_MJ_JOINTUPLES;
				}
				else if (compareResult < 0)
					node->mj_JoinState = EXEC_MJ_SKIPOUTER_ADVANCE;
				else
					/* compareResult > 0 */
					node->mj_JoinState = EXEC_MJ_SKIPINNER_ADVANCE;
				break;

				/*
				 * SKIPOUTER_ADVANCE: advance over an outer tuple that is
				 * known not to join to any inner tuple.
				 *
				 * Before advancing, we check to see if we must emit an
				 * outer-join fill tuple for this outer tuple.
				 */
			case EXEC_MJ_SKIPOUTER_ADVANCE:
				MJ_printf("ExecMergeJoin: EXEC_MJ_SKIPOUTER_ADVANCE\n");

				if (doFillOuter && !node->mj_MatchedOuter)
				{
					/*
					 * Generate a fake join tuple with nulls for the inner
					 * tuple, and return it if it passes the non-join quals.
					 */
					TupleTableSlot *result;

					node->mj_MatchedOuter = true;	/* do it only once */

					result = MJFillOuter(node);
					if (result)
						return result;
				}

				/*
				 * now we get the next outer tuple, if any
				 */
				outerTupleSlot = ExecProcNode(outerPlan);
				node->mj_OuterTupleSlot = outerTupleSlot;
				MJ_DEBUG_PROC_NODE(outerTupleSlot);
				node->mj_MatchedOuter = false;

				/* Compute join values and check for unmatchability */
				switch (MJEvalOuterValues(node))
				{
					case MJEVAL_MATCHABLE:
						/* Go test the new tuple against the current inner */
						node->mj_JoinState = EXEC_MJ_SKIP_TEST;
						break;
					case MJEVAL_NONMATCHABLE:
						/* Can't match, so fetch next outer tuple */
						node->mj_JoinState = EXEC_MJ_SKIPOUTER_ADVANCE;
						break;
					case MJEVAL_ENDOFJOIN:
						/* No more outer tuples */
						MJ_printf("ExecMergeJoin: end of outer subplan\n");
						innerTupleSlot = node->mj_InnerTupleSlot;
						if (doFillInner && !TupIsNull(innerTupleSlot))
						{
							/*
							 * Need to emit right-join tuples for remaining
							 * inner tuples.
							 */
							node->mj_JoinState = EXEC_MJ_ENDOUTER;
							break;
						}
						/* Otherwise we're done. */
						return NULL;
				}
				break;

				/*
				 * SKIPINNER_ADVANCE: advance over an inner tuple that is
				 * known not to join to any outer tuple.
				 *
				 * Before advancing, we check to see if we must emit an
				 * outer-join fill tuple for this inner tuple.
				 */
			case EXEC_MJ_SKIPINNER_ADVANCE:
				MJ_printf("ExecMergeJoin: EXEC_MJ_SKIPINNER_ADVANCE\n");

				if (doFillInner && !node->mj_MatchedInner)
				{
					/*
					 * Generate a fake join tuple with nulls for the outer
					 * tuple, and return it if it passes the non-join quals.
					 */
					TupleTableSlot *result;

					node->mj_MatchedInner = true;	/* do it only once */

					result = MJFillInner(node);
					if (result)
						return result;
				}

				/* Mark before advancing, if wanted */
				if (node->mj_ExtraMarks)
					ExecMarkPos(innerPlan);

				/*
				 * now we get the next inner tuple, if any
				 */
				innerTupleSlot = ExecProcNode(innerPlan);
				node->mj_InnerTupleSlot = innerTupleSlot;
				MJ_DEBUG_PROC_NODE(innerTupleSlot);
				node->mj_MatchedInner = false;

				/* Compute join values and check for unmatchability */
				switch (MJEvalInnerValues(node, innerTupleSlot))
				{
					case MJEVAL_MATCHABLE:
						/* proceed to compare it to the current outer */
						node->mj_JoinState = EXEC_MJ_SKIP_TEST;
						break;
					case MJEVAL_NONMATCHABLE:

						/*
						 * current inner can't possibly match any outer;
						 * better to advance the inner scan than the outer.
						 */
						node->mj_JoinState = EXEC_MJ_SKIPINNER_ADVANCE;
						break;
					case MJEVAL_ENDOFJOIN:
						/* No more inner tuples */
						MJ_printf("ExecMergeJoin: end of inner subplan\n");
						outerTupleSlot = node->mj_OuterTupleSlot;
						if (doFillOuter && !TupIsNull(outerTupleSlot))
						{
							/*
							 * Need to emit left-join tuples for remaining
							 * outer tuples.
							 */
							node->mj_JoinState = EXEC_MJ_ENDINNER;
							break;
						}
						/* Otherwise we're done. */
						return NULL;
				}
				break;

				/*
				 * EXEC_MJ_ENDOUTER means we have run out of outer tuples, but
				 * are doing a right/full join and therefore must null-fill
				 * any remaining unmatched inner tuples.
				 */
			case EXEC_MJ_ENDOUTER:
				MJ_printf("ExecMergeJoin: EXEC_MJ_ENDOUTER\n");

				Assert(doFillInner);

				if (!node->mj_MatchedInner)
				{
					/*
					 * Generate a fake join tuple with nulls for the outer
					 * tuple, and return it if it passes the non-join quals.
					 */
					TupleTableSlot *result;

					node->mj_MatchedInner = true;	/* do it only once */

					result = MJFillInner(node);
					if (result)
						return result;
				}

				/* Mark before advancing, if wanted */
				if (node->mj_ExtraMarks)
					ExecMarkPos(innerPlan);

				/*
				 * now we get the next inner tuple, if any
				 */
				innerTupleSlot = ExecProcNode(innerPlan);
				node->mj_InnerTupleSlot = innerTupleSlot;
				MJ_DEBUG_PROC_NODE(innerTupleSlot);
				node->mj_MatchedInner = false;

				if (TupIsNull(innerTupleSlot))
				{
					MJ_printf("ExecMergeJoin: end of inner subplan\n");
					return NULL;
				}

				/* Else remain in ENDOUTER state and process next tuple. */
				break;

				/*
				 * EXEC_MJ_ENDINNER means we have run out of inner tuples, but
				 * are doing a left/full join and therefore must null- fill
				 * any remaining unmatched outer tuples.
				 */
			case EXEC_MJ_ENDINNER:
				MJ_printf("ExecMergeJoin: EXEC_MJ_ENDINNER\n");

				Assert(doFillOuter);

				if (!node->mj_MatchedOuter)
				{
					/*
					 * Generate a fake join tuple with nulls for the inner
					 * tuple, and return it if it passes the non-join quals.
					 */
					TupleTableSlot *result;

					node->mj_MatchedOuter = true;	/* do it only once */

					result = MJFillOuter(node);
					if (result)
						return result;
				}

				/*
				 * now we get the next outer tuple, if any
				 */
				outerTupleSlot = ExecProcNode(outerPlan);
				node->mj_OuterTupleSlot = outerTupleSlot;
				MJ_DEBUG_PROC_NODE(outerTupleSlot);
				node->mj_MatchedOuter = false;

				if (TupIsNull(outerTupleSlot))
				{
                    if (TupIsComplete(outerTupleSlot)) 
                            node->isComplete = true;
					MJ_printf("ExecMergeJoin: end of outer subplan\n");
					return NULL;
				}

				/* Else remain in ENDINNER state and process next tuple. */
				break;

				/*
				 * broken state value?
				 */
			default:
				elog(ERROR, "unrecognized mergejoin state: %d",
					 (int) node->mj_JoinState);
		}
	}
}


/* ----------------------------------------------------------------
 *		ExecMergeJoinInc
 * ----------------------------------------------------------------
 */

TupleTableSlot *
ExecMergeJoinInc(PlanState *pstate)
{
	MergeJoinState *node = castNode(MergeJoinState, pstate);
    TupleTableSlot * result_slot = ExecMergeJoin(pstate); 
    if (!TupIsNull(result_slot)) 
    {
        return result_slot; 
    }
    else
    {
        result_slot = node->js.ps.ps_ProjInfo->pi_state.resultslot; 
        ExecClearTuple(result_slot);
        if (node->isComplete) 
        {
            MarkTupComplete(result_slot, true);
        }
        else 
        {
            /* reset the states of mergejoin */
        	ExecClearTuple(node->mj_MarkedTupleSlot);
	        node->mj_JoinState = EXEC_MJ_INITIALIZE_OUTER;
        	node->mj_MatchedOuter = false;
	        node->mj_MatchedInner = false;
	        node->mj_OuterTupleSlot = NULL;
	        node->mj_InnerTupleSlot = NULL;
            ExecReScan(innerPlanState(node));

            MarkTupComplete(result_slot, false);
        }
        return result_slot; 
    }
}


/* ----------------------------------------------------------------
 *		ExecInitMergeJoinInc
 * ----------------------------------------------------------------
 */
void 
ExecInitMergeJoinInc(MergeJoinState *mergestate)
{
    mergestate->isComplete = false;
}


void
ExecResetMergeJoinState(MergeJoinState * node)
{    
    PlanState *innerPlan; 
    PlanState *outerPlan;

    innerPlan = outerPlanState(outerPlanState(innerPlanState(node))); /* Skip Material and Sort nodes */
    outerPlan = outerPlanState(outerPlanState(node));                  /* Skip Sort node */

    ExecResetState(innerPlan); 
    ExecResetState(outerPlan); 
}

/* ----------------------------------------------------------------
 *		ExecInitMergeJoinDelta
 *
 *		init merge join state for delta processing
 * ----------------------------------------------------------------
 */

void
ExecInitMergeJoinDelta(MergeJoinState * node)
{
    PlanState *innerPlan; 
    PlanState *outerPlan;

    innerPlan = outerPlanState(outerPlanState(innerPlanState(node))); /* Skip Material and Sort nodes */
    outerPlan = outerPlanState(outerPlanState(node));                  /* Skip Sort node */

    ExecInitDelta(innerPlan); 
    ExecInitDelta(outerPlan); 
}
