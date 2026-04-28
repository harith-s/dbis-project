#include "postgres.h"
#include "autoindex.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "nodes/makefuncs.h"
#include "storage/ipc.h"
#include "storage/shmem.h"
#include "storage/lwlock.h"
#include "nodes/nodeFuncs.h"
#include "nodes/parsenodes.h"
#include "catalog/pg_index.h"
#include "utils/syscache.h"
#include "catalog/index.h"
#include "utils/selfuncs.h"
#include "access/htup_details.h"

// for index removal on bulk insert/delete

#include "autoindex.h"
#include "access/genam.h"
#include "access/table.h"
#include "executor/spi.h"
#include "utils/fmgroids.h"
#include "utils/snapmgr.h"
#include "tcop/utility.h"

static HTAB *AutoIndexHash = NULL;

//this checks if an index already exists on this relation and attribute
static bool IndexAlreadyExists(Oid relid, AttrNumber attnum)
{
    List *indexList;
    ListCell *lc;

    indexList = RelationGetIndexList(relation_open(relid, AccessShareLock));

    foreach(lc, indexList)
    {
        Oid indexOid = lfirst_oid(lc);
        HeapTuple indexTuple;
        Form_pg_index indexForm;

        indexTuple = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(indexOid));
        if (!HeapTupleIsValid(indexTuple))
            continue;

        indexForm = (Form_pg_index) GETSTRUCT(indexTuple);

        for (int i = 0; i < indexForm->indnatts; i++)
        {
            if (indexForm->indkey.values[i] == attnum)
            {
                ReleaseSysCache(indexTuple);
                return true;
            }
        }

        ReleaseSysCache(indexTuple);
    }

    return false;
}

static double EstimateEqualitySelectivity(Oid relid, AttrNumber attnum)
{
    // right now this is just the reciprocal of num of distinct vals but could
    // use some big brain momints and try to use the histogram
    HeapTuple statsTuple;
    Form_pg_statistic stats;
    double selectivity = 0.1;

    statsTuple = SearchSysCache3(STATRELATTINH,
                                ObjectIdGetDatum(relid),
                                Int16GetDatum(attnum),
                                BoolGetDatum(false));

    if (!HeapTupleIsValid(statsTuple)) return selectivity;

    stats = (Form_pg_statistic) GETSTRUCT(statsTuple);

    if (stats->stadistinct > 0) selectivity = 1.0 / stats->stadistinct;
    else if (stats->stadistinct < 0) selectivity = -stats->stadistinct;

    ReleaseSysCache(statsTuple);
    return selectivity;
}

static bool IsColumnSelective(Oid relid, AttrNumber attnum)
{
    HeapTuple statsTuple;
    Form_pg_statistic stats;
    bool result = true;

    statsTuple = SearchSysCache3(STATRELATTINH,
                                ObjectIdGetDatum(relid),
                                Int16GetDatum(attnum),
                                BoolGetDatum(false));

    if (HeapTupleIsValid(statsTuple))
    {
        stats = (Form_pg_statistic) GETSTRUCT(statsTuple);

        // if a lot of tuples have the smae value for this attribute 
        // then it is not very selective and creating an index on it would be useless
        if (stats->stadistinct > 0 && stats->stadistinct < 10)
            result = false;

        ReleaseSysCache(statsTuple);
    }

    return result;
}

Size AutoIndexShmemSize(void) {
    return hash_estimate_size(1024, sizeof(AutoIndexEntry));
}

void AutoIndexShmemInit(void) {
    HASHCTL info;
    info.keysize = sizeof(AutoIndexKey);
    info.entrysize = sizeof(AutoIndexEntry);
    
    // Corrected: ShmemInitHash takes 4 arguments in this version
    AutoIndexHash = ShmemInitHash("AutoIndex Hash",
                                  1024,
                                  &info,
                                  HASH_ELEM | HASH_BLOBS);
}

void TrackEqualityPredicate(Oid relid, AttrNumber attnum) {
    if (relid < (Oid) 16384) {
        ereport(LOG, (errmsg("AutoIndex: Skipping system catalog %d", relid)));
        return;
    }

    AutoIndexKey key;
    AutoIndexEntry *entry;
    bool found;
    uint32 current_count;

    if (!AutoIndexHash)
        return;

    key.relid = relid;
    key.attnum = attnum;

    LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);

    entry = (AutoIndexEntry *) hash_search(AutoIndexHash, &key, HASH_ENTER, &found);

    if (!found) {
        entry->scan_count = 1;
    } else {
        entry->scan_count++;
    }

    current_count = entry->scan_count;
    LWLockRelease(AddinShmemInitLock);

    uint32 dynamic_threshold = AUTOINDEX_THRESHOLD;

    // using relative counts instead of absolute to determine when
    if (relid % 2 == 0) dynamic_threshold *= 2;
    
    // if (current_count >= dynamic_threshold)
    if (current_count == AUTOINDEX_THRESHOLD) {
        LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);
        entry->scan_count = 0; 
        LWLockRelease(AddinShmemInitLock);
        ExecuteAutoIndex(relid, attnum);
    }
}

void TrackEqualityPredicate_New(Oid relid, AttrNumber attnum)
{
    if (relid < (Oid) 16384) return;
    if (!AutoIndexHash) return;
    if (IndexAlreadyExists(relid, attnum)) return;

    if (!IsColumnSelective(relid, attnum)) return;

    AutoIndexKey key;
    AutoIndexEntry *entry;
    bool found;
    uint32 current_count;

    key.relid = relid;
    key.attnum = attnum;

    LWLockAcquire(LWLockNamedTranche("AutoIndexLock"), LW_EXCLUSIVE);

    entry = (AutoIndexEntry *) hash_search(AutoIndexHash, &key, HASH_ENTER, &found);

    if (!found) entry->scan_count = 1;
    else entry->scan_count++;

    current_count = entry->scan_count;

    LWLockRelease(LWLockNamedTranche("AutoIndexLock"));

    uint32 dynamic_threshold = AUTOINDEX_THRESHOLD;
    if (relid % 2 == 0)
        dynamic_threshold *= 2;

    if (current_count >= dynamic_threshold)
    {
        LWLockAcquire(LWLockNamedTranche("AutoIndexLock"), LW_EXCLUSIVE);
        entry->scan_count = 0;
        LWLockRelease(LWLockNamedTranche("AutoIndexLock"));
        ExecuteAutoIndex(relid, attnum);
    }
}

void ExecuteAutoIndex(Oid relid, AttrNumber attnum)
{
    char       *relname_raw;
    char       *attname_raw;
    char       *relname;
    char       *attname;
    IndexStmt  *stmt;
    IndexElem  *iparam;

    relname_raw = get_rel_name(relid);
    attname_raw = get_attname(relid, attnum, false);

    if (!relname_raw || !attname_raw)
        return;

    relname = pstrdup(relname_raw);
    attname = pstrdup(attname_raw);

    ereport(LOG,
    (errmsg("AutoIndex: Creating index on %s(%s) after threshold trigger",
            relname, attname)));                    

    /* 2. Construct the Node structures */
    stmt = makeNode(IndexStmt);
    stmt->idxname = NULL;
    stmt->relation = makeRangeVar(NULL, relname, -1);
    stmt->accessMethod = "btree";
    stmt->tableSpace = NULL;

    iparam = makeNode(IndexElem);
    iparam->name = attname;
    iparam->expr = NULL;
    iparam->indexcolname = NULL;
    iparam->collation = NIL;
    iparam->opclass = NIL;
    iparam->opclassopts = NIL;
    iparam->ordering = SORTBY_DEFAULT;
    iparam->nulls_ordering = SORTBY_NULLS_DEFAULT;
    iparam->location = -1;

    stmt->indexParams = list_make1(iparam);
    stmt->options = NIL;
    stmt->whereClause = NULL;
    stmt->excludeOpNames = NIL;
    stmt->idxcomment = "Auto-generated by SeqScan tracker";
    stmt->indexOid = InvalidOid;
    stmt->unique = false;
    stmt->primary = false; 
    stmt->isconstraint = false;
    stmt->deferrable = false;
    stmt->initdeferred = false;
    stmt->transformed = false;
    
    // concurrent being true would be ideal to avoid issues with the shared memory
    // but causes issues when it is 
    stmt->concurrent = false; 
    stmt->if_not_exists = true;

    PG_TRY();
    {
        // if we can ensure that DefineIndex won't fail due to concurrent transaction issues
        // we can set cincurrent ti false and directly call DefineIndex
        DefineIndex(NULL, relid, stmt, InvalidOid, InvalidOid, InvalidOid, 
                    0, false, false, false, false, true);
    }
    PG_CATCH();
    {
        FlushErrorState();
        ereport(WARNING,
                (errmsg("AutoIndex: Execution failed. Table may be locked by current scan.")));
    }
    PG_END_TRY();
}

int autoindex_bulk_threshold = 1000;

/*
 * Collect all droppable indexes on this relation, drop them,
 * and return a list of SavedIndexInfo so we can recreate later.
 *
 * We skip: primary keys, unique indexes (dropping would break constraints),
 * and indexes used by active constraints.
 */ 
List * AutoIndexDropForBulk(Relation rel)
{
    List       *saved = NIL;
    List       *indexOids;
    ListCell   *lc;

    /* Get all index OIDs for this relation */
    indexOids = RelationGetIndexList(rel);

    foreach(lc, indexOids)
    {
        Oid             indexOid = lfirst_oid(lc);
        HeapTuple       indexTuple;
        Form_pg_index   indexForm;
        SavedIndexInfo *info;

        indexTuple = SearchSysCache1(INDEXRELID,
                                     ObjectIdGetDatum(indexOid));
        if (!HeapTupleIsValid(indexTuple))
            continue;

        indexForm = (Form_pg_index) GETSTRUCT(indexTuple);

        /* Skip primary keys and unique indexes — dropping them
         * would silently remove constraint enforcement            */
        if (indexForm->indisprimary || indexForm->indisunique)
        {
            ReleaseSysCache(indexTuple);
            continue;
        }

        /* Skip invalid or not-yet-ready indexes */
        if (!indexForm->indisvalid || !indexForm->indisready)
        {
            ReleaseSysCache(indexTuple);
            continue;
        }

        /* Save the full CREATE INDEX statement from pg_indexes.
         * This is the safest way to recreate — handles expressions,
         * partials, custom opclasses, storage params, etc.         */
        info = palloc0(sizeof(SavedIndexInfo));
        info->indexOid  = indexOid;
        info->relOid    = RelationGetRelid(rel);
        info->indexName = pstrdup(get_rel_name(indexOid));
        info->isUnique  = indexForm->indisunique;
        info->isPrimary = indexForm->indisprimary;

        /* Pull the full definition string from pg_indexes view */
        info->indexDef  = GetIndexDef(indexOid); /* see below */

        ReleaseSysCache(indexTuple);

        saved = lappend(saved, info);
    }

    /* Now drop all collected indexes */
    foreach(lc, saved)
    {
        SavedIndexInfo *info = (SavedIndexInfo *) lfirst(lc);
        index_drop(info->indexOid, false /* concurrent */, true /* concurrent ok */);
    }

    return saved;
}

/*
 * Fetch the full CREATE INDEX statement for an index OID.
 * pg_get_indexdef() is the canonical way to do this.
 */
static char * GetIndexDef(Oid indexOid)
{
    return TextDatumGetCString(
        DirectFunctionCall2(pg_get_indexdef_ext,
                            ObjectIdGetDatum(indexOid),
                            BoolGetDatum(false)));
}

/*
 * Recreate all saved indexes after the bulk operation completes.
 * Uses the saved CREATE INDEX string — this handles all index types
 * correctly without needing to reconstruct from catalog columns.
 */
void AutoIndexRecreate(List *savedIndexes)
{
    ListCell *lc;

    foreach(lc, savedIndexes)
    {
        SavedIndexInfo *info = (SavedIndexInfo *) lfirst(lc);
        int             ret;

        /* Execute the saved CREATE INDEX statement via SPI */
        SPI_connect();
        ret = SPI_execute(info->indexDef, false, 0);
        SPI_finish();

        if (ret != SPI_OK_UTILITY)
            ereport(ERROR,
                    (errmsg("autoindex: failed to recreate index \"%s\"",
                            info->indexName)));
    }
}

/*
 * Called in the abort path — attempt best-effort recreation.
 * We suppress errors here because the transaction is already
 * rolling back; we don't want to mask the original error.
 */
void AutoIndexRecreateOnAbort(List *savedIndexes)
{
    ListCell *lc;

    foreach(lc, savedIndexes)
    {
        SavedIndexInfo *info = (SavedIndexInfo *) lfirst(lc);

        PG_TRY();
        {
            SPI_connect();
            SPI_execute(info->indexDef, false, 0);
            SPI_finish();
        }
        PG_CATCH();
        {
            /* Log but don't rethrow — the original error takes priority */
            elog(WARNING, "autoindex: could not recreate index \"%s\" during abort",
                 info->indexName);
            FlushErrorState();
        }
        PG_END_TRY();
    }
}