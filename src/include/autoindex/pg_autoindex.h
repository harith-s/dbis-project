#ifndef PG_AUTOINDEX_H
#define PG_AUTOINDEX_H

#include "postgres.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "storage/subsystems.h"
#include "nodes/nodes.h"
#include <stdint.h>

#include "storage/block.h"

#define AUTOINDEX_MAX_ENTRIES  1024
#define DROPINDEX_MAX_ENTRIES 1024
#define DROPINDEX_THRESHOLD   5
#define AUTOINDEX_MAX_COLS     32

typedef struct AutoindexKey {
    Oid     reloid;
    int16   attnums[AUTOINDEX_MAX_COLS];
    int     ncolumns;
} AutoindexKey;

typedef struct AutoindexEntry {
    Oid          dboid;
    AutoindexKey key;
    bool         in_use;
    bool         index_triggered;
    int64        scan_count;
    int64_t drop_count;
    double  accumulated_cost;
    double  build_cost;
} AutoindexEntry;

typedef struct DropindexEntry {
    Oid     dboid;
    Oid     reloid;
    int16   attnums[AUTOINDEX_MAX_COLS];
    int     ncolumns;
    bool    in_use;
    bool    index_dropped;
    int64   update_count;
    double  maintenance_cost;
} DropindexEntry;

typedef struct AutoindexSharedState {
    LWLockPadded  lock;
    int           num_entries;
    AutoindexEntry entries[AUTOINDEX_MAX_ENTRIES];
} AutoindexSharedState;

typedef struct DropindexSharedState {
    LWLockPadded  lock;
    int           num_entries;
    DropindexEntry entries[DROPINDEX_MAX_ENTRIES];
} DropindexSharedState;

extern bool autoindex_enabled;
extern AutoindexSharedState *AutoindexShmem;
extern const ShmemCallbacks AutoindexShmemCallbacks;

extern DropindexSharedState *DropindexShmem;
extern const ShmemCallbacks DropindexShmemCallbacks;

extern void auto_composite_index_record_scan(Oid dboid, Oid reloid, int16 *attnos, int num_attnos, Cost scan_count, double build_cost);
extern void AutoindexRegister(void);
extern void AutoindexWorkerMain(Datum main_arg);

extern void dropindex_record_scan(Oid dboid, Oid reloid, BlockNumber relpages);
extern void DropindexRegister(void);
extern void DropindexWorkerMain(Datum main_arg);

extern void autoindex_register_gucs(void);

#endif