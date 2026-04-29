#ifndef PG_AUTOINDEX_H
#define PG_AUTOINDEX_H

#include "postgres.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "storage/subsystems.h"

#define AUTOINDEX_MAX_ENTRIES  1024
#define AUTOINDEX_THRESHOLD    5
#define DROPINDEX_MAX_ENTRIES 1024
#define DROPINDEX_THRESHOLD   5

typedef struct AutoindexEntry {
    Oid     dboid;
    Oid     reloid;
    int16   attno;
    bool    in_use;
    bool    index_triggered;
    int64   scan_count;
} AutoindexEntry;

typedef struct DropindexEntry {
    Oid     dboid;
    Oid     reloid;
    bool    in_use;
    bool    index_dropped;
    int64   update_count;
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

extern AutoindexSharedState *AutoindexShmem;
extern const ShmemCallbacks AutoindexShmemCallbacks;

extern DropindexSharedState *DropindexShmem;
extern const ShmemCallbacks DropindexShmemCallbacks;

extern void autoindex_record_scan(Oid dboid, Oid reloid, int16 attno);
extern void AutoindexRegister(void);
extern void AutoindexWorkerMain(Datum main_arg);

extern void dropindex_record_scan(Oid dboid, Oid reloid);
extern void DropindexRegister(void);
extern void DropindexWorkerMain(Datum main_arg);

#endif