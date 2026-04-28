#ifndef PG_AUTOINDEX_H
#define PG_AUTOINDEX_H

#include "postgres.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "storage/subsystems.h"

#define AUTOINDEX_MAX_ENTRIES  1024
#define AUTOINDEX_THRESHOLD    5

typedef struct AutoindexEntry {
    Oid     dboid;
    Oid     reloid;
    int16   attno;
    bool    in_use;
    bool    index_triggered;
    int64   scan_count;
} AutoindexEntry;

typedef struct AutoindexSharedState {
    LWLockPadded  lock;
    int           num_entries;
    AutoindexEntry entries[AUTOINDEX_MAX_ENTRIES];
} AutoindexSharedState;

extern AutoindexSharedState *AutoindexShmem;
extern const ShmemCallbacks AutoindexShmemCallbacks;

extern void autoindex_record_scan(Oid dboid, Oid reloid, int16 attno);
extern void AutoindexRegister(void);
extern void AutoindexWorkerMain(Datum main_arg);
extern void AutoindexRegister(void);

#endif