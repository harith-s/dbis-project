#ifndef AUTOINDEX_H
#define AUTOINDEX_H

#include "postgres.h"
#include "utils/hsearch.h"
#include "nodes/execnodes.h"

/* The threshold of sequential scans before we trigger an index build */
#define AUTOINDEX_THRESHOLD 5

/* Shared memory hash table key */
typedef struct AutoIndexKey {
    Oid         relid;      /* Table OID */
    AttrNumber  attnum;     /* Column Attribute Number */
} AutoIndexKey;

/* Shared memory hash table entry */
typedef struct AutoIndexEntry {
    AutoIndexKey key;
    uint32       scan_count; /* Number of times this column was scanned with '=' */
} AutoIndexEntry;

/* Function prototypes */
extern Size AutoIndexShmemSize(void);
extern void AutoIndexShmemInit(void);
extern void TrackEqualityPredicate(Oid relid, AttrNumber attnum);
extern void ExecuteAutoIndex(Oid relid, AttrNumber attnum);

#endif /* AUTOINDEX_H */