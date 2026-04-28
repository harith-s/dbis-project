#ifndef AUTOINDEX_H
#define AUTOINDEX_H

#include "postgres.h"
#include "utils/hsearch.h"
#include "nodes/execnodes.h"

// The threshold of sequential scans before we trigger an index build 
#define AUTOINDEX_THRESHOLD 5

// Shared memory hash table key
typedef struct AutoIndexKey {
    Oid         relid;      // Table OID
    AttrNumber  attnum;     // Column Attribute Number
} AutoIndexKey;

// Shared memory hash table entry
typedef struct AutoIndexEntry {
    AutoIndexKey key;
    uint32       scan_count; // Number of times this column was scanned with '='
} AutoIndexEntry;

// Function prototypes
extern Size AutoIndexShmemSize(void);
extern void AutoIndexShmemInit(void);
extern void TrackEqualityPredicate(Oid relid, AttrNumber attnum);
extern void ExecuteAutoIndex(Oid relid, AttrNumber attnum);

// to trigger index removal on bulk insert/delete

#include "postgres.h"
#include "nodes/pg_list.h"
#include "utils/relcache.h"

typedef struct SavedIndexInfo
{
    Oid indexOid;
    char *indexName;
    Oid relOid;
    Oid accessMethod; // btree, hash, gin, etc
    List *indexColNums; // column numbers
    List *indexExprs; // expression indexes
    List *indexPreds; // partial index WHERE clause 
    bool isUnique;
    bool isPrimary;
    bool isConcurrent; // recreate with CONCURRENTLY?
    char *indexDef; // full CREATE INDEX string, from pg_indexes
} SavedIndexInfo;

extern int  autoindex_bulk_threshold;

extern List *AutoIndexDropForBulk(Relation rel);
extern void  AutoIndexRecreate(List *savedIndexes);
extern void  AutoIndexRecreateOnAbort(List *savedIndexes);

#endif 