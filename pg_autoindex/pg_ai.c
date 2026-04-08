#include "postgres.h"
#include "fmgr.h"
#include "optimizer/plancat.h"
#include "nodes/pathnodes.h"
#include "utils/lsyscache.h"    /* get_atttype(), get_opfamily_oper() */
#include "utils/syscache.h"     /* SearchSysCache1, AMNAME */
#include "catalog/pg_am.h"      /* BTREE_AM_OID */
#include "catalog/pg_opfamily.h"
#include "catalog/pg_type.h"
#include "utils/rel.h"
#include "access/htup_details.h"

PG_MODULE_MAGIC;

static get_relation_info_hook_type prev_get_relation_info_hook = NULL;

/* ----------------------------------------------------------------
 * inject_hypothetical_index
 *
 * Builds one fake IndexOptInfo and appends it to rel->indexlist.
 * rel        - the RelOptInfo the planner built for this table
 * col_attno  - 1-based attribute number of the column to index
 *              (e.g. 2 = second column in the table)
 * ---------------------------------------------------------------- */
static void
inject_hypothetical_index(RelOptInfo *rel, int col_attno)
{
    IndexOptInfo   *info;
    Oid             col_type;

    /* --- 1. Allocate in the planner's memory context --- */
    info = makeNode(IndexOptInfo);   /* zero-fills + sets NodeTag */

    /* --- 2. Identity fields --- */
    /*
     * We use InvalidOid here to signal "not a real index".
     * HypoPG uses a dedicated OID range starting at 9000000.
     * For now InvalidOid is fine — indxpath.c doesn't dereference
     * indexoid for anything critical during path generation.
     */
    info->indexoid       = InvalidOid;
    info->reltablespace  = rel->reltablespace; /* same tablespace as the table */
    info->rel            = rel;                /* back-pointer */
    info->relam          = BTREE_AM_OID;       /* btree = 403 in pg_am */
    info->hypothetical   = true;               /* flag — add to IndexOptInfo struct */

    /* --- 3. Size estimates ---
     *
     * pages:  estimate index size as ~20% of the table's pages.
     *         Real btree indexes on a single int4 column are roughly
     *         that fraction of the heap for typical row widths.
     * tuples: same row count as the table.
     * tree_height: -1 means "unknown" — the cost model handles this
     *         by assuming 1 level (leaf-only scan), which is fine for
     *         estimates on a table we haven't built yet.
     */
    info->pages       = Max(1, rel->pages * 0.20);
    info->tuples      = rel->tuples;
    info->tree_height = -1;

    /* --- 4. Column mapping ---
     *
     * ncolumns:   how many index columns (1 for a single-column index)
     * indexkeys:  array of attribute numbers. 0 means expression index
     *             (not supported here). Use palloc0 so unused slots are 0.
     */
    info->ncolumns  = 1;
    info->indexkeys = (int *) palloc0(sizeof(int) * info->ncolumns);
    info->indexkeys[0] = col_attno;

    /* --- 5. Operator family and input type ---
     *
     * The planner uses opfamily to check if a WHERE clause operator
     * can be satisfied by this index. For btree on a scalar type
     * (int4, text, timestamp, etc.) we look up the default btree
     * opfamily for the column's type.
     *
     * get_opclass_family / get_opclass_input_type are the syscache
     * helpers for this. We need the OID of the default opclass for
     * the column type in the btree AM.
     */
    col_type = get_atttype(rel->relid, col_attno);

    {
        Oid     opclass  = GetDefaultOpClass(col_type, BTREE_AM_OID);
        Oid     opfamily = InvalidOid;
        Oid     opcintype = InvalidOid;

        if (OidIsValid(opclass))
        {
            opfamily  = get_opclass_family(opclass);
            opcintype = get_opclass_input_type(opclass);
        }

        info->opfamily  = (Oid *) palloc(sizeof(Oid) * info->ncolumns);
        info->opcintype = (Oid *) palloc(sizeof(Oid) * info->ncolumns);
        info->opfamily[0]  = opfamily;
        info->opcintype[0] = opcintype;
    }

    /* --- 6. Sort operators (needed for ORDER BY pushdown to index) ---
     *
     * sortopfamily: same opfamily used for sort comparison
     * reverse_sort: false = ASC, true = DESC
     * nulls_first:  how NULLs sort (false = NULLS LAST for ASC)
     * canreturn:    index-only scan support — false for our hypo index
     */
    info->sortopfamily = (Oid *)  palloc(sizeof(Oid)  * info->ncolumns);
    info->reverse_sort = (bool *) palloc0(sizeof(bool) * info->ncolumns);
    info->nulls_first  = (bool *) palloc0(sizeof(bool) * info->ncolumns);
    info->canreturn    = (bool *) palloc0(sizeof(bool) * info->ncolumns);

    info->sortopfamily[0] = info->opfamily[0]; /* reuse btree opfamily */
    info->reverse_sort[0] = false;             /* ASC */
    info->nulls_first[0]  = false;             /* NULLS LAST */
    info->canreturn[0]    = false;             /* no index-only scans */

    /* --- 7. Collations ---
     *
     * For non-text types this is InvalidOid (no collation).
     * For text/varchar you'd use DEFAULT_COLLATION_OID.
     * We use InvalidOid here — the planner treats this as "no
     * collation constraint", which works for numeric types.
     */
    info->indexcollations    = (Oid *) palloc0(sizeof(Oid) * info->ncolumns);
    info->indexcollations[0] = InvalidOid;

    /* --- 8. Behavior flags --- */
    info->unique       = false;  /* not enforcing uniqueness */
    info->immediate    = true;   /* unique constraint is immediate (irrelevant if !unique) */
    info->predOK       = false;  /* no partial index predicate */
    info->indpred      = NIL;    /* no partial index predicate expression */
    info->indextlist   = NIL;    /* no included (non-key) columns */

    /*
     * indexCorrelation: THE most important cost knob.
     *
     * 1.0  = index order perfectly matches heap order (freshly clustered)
     *        → planner strongly prefers index scan (random I/O is minimal)
     * 0.0  = completely random (typical heap after many updates)
     *        → planner may prefer seqscan for low-selectivity queries
     *
     * For a brand-new hypothetical index on an unclustered table,
     * 0.0 is the conservative/honest choice. Use 1.0 if you know
     * the table was recently clustered or is insert-only (append-only pattern).
     *
     * This value is read directly by cost_index() in costsize.c.
     */
    info->indexCorrelation = 0.0;

    /* --- 9. AM capability flags ---
     *
     * These come from pg_am / the AM's amroutine in a real index.
     * For btree, the important ones:
     */
    info->amcanorderbyop = false; /* btree can't order by operator result */
    info->amoptionalkey  = true;  /* can scan without supplying all key cols */
    info->amsearcharray  = true;  /* supports ScalarArrayOp (col = ANY(...)) */
    info->amsearchnulls  = true;  /* can search for IS NULL / IS NOT NULL */
    info->amhasgettuple  = true;  /* supports index scan */
    info->amhasgetbitmap = true;  /* supports bitmap index scan */

    /* --- 10. Append to rel->indexlist --- */
    rel->indexlist = lappend(rel->indexlist, info);
}


/* ----------------------------------------------------------------
 * The hook itself
 * ---------------------------------------------------------------- */
static void
pg_autoindex_get_relation_info(PlannerInfo *root,
                                Oid relationObjectId,
                                bool inhparent,
                                RelOptInfo *rel)
{
    /* Always chain to load real indexes first */
    if (prev_get_relation_info_hook)
        prev_get_relation_info_hook(root, relationObjectId, inhparent, rel);

    /*
     * For now: only inject on a specific table for testing.
     * Replace "myschema.mytable" with your actual test table.
     * Later this will be driven by your candidate list.
     */
    Oid target_table = RelnameGetRelid("orders");  /* example */

    if (relationObjectId == target_table)
    {
        /*
         * Inject a hypothetical btree index on column 3
         * (3rd column of the "orders" table — check with \d orders).
         */
        inject_hypothetical_index(rel, 3);
    }
}

/*
//to cgeck
-- Load the extension for this session
LOAD 'pg_autoindex';

-- Check the plan WITHOUT your hypothetical index
SET pg_autoindex.enabled = off;  -- (we'll add this GUC later)
EXPLAIN SELECT * FROM orders WHERE customer_id = 42;
-- Expect: Seq Scan

-- Check the plan WITH your hypothetical index
SET pg_autoindex.enabled = on;
EXPLAIN SELECT * FROM orders WHERE customer_id = 42;
-- Expect: Index Scan using <hypothetical> on orders

*/