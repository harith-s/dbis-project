# pg_autoindex Implementation Plan

## Problem Statement

Modify PostgreSQL's sequential scan code to track full relation scans that have an
equality predicate on a column. When the accumulated cost of those scans exceeds the
estimated cost of building an index, automatically create the index in a background
process.

This solves the common scenario where an application performs poorly because a
needed index was never created — the system detects and fixes this automatically.

---

## Design Decisions

- **Start simple**: single-column, `Var = Const` equality predicates, SeqScan only.
- **Separate process**: index creation happens in a background worker, not the
  scanning backend (index builds are expensive and should not block queries).
- **Persistence**: scan counts survive server restarts via a binary file.
- **Single background worker**: one worker handles all index creation for one
  database. Multi-database support is a future extension.
- **Hardcoded cost constants**: fake placeholder values, clearly documented.
  Replace with real estimates (proportional to `relpages`) in a later pass.

---

## Architecture

```
SeqScan execution (any backend)
      │
      │  ExecInitSeqScan  → analyze quals, find Var=Const equality attno
      │                     check no existing index on that column
      │  ExecEndSeqScan   → increment shmem counter for (dboid, reloid, attno)
      │  ExecReScanSeqScan→ same (each rescan is a full scan too)
      ▼
┌──────────────────────────────────────────────┐
│  Shared Memory: AutoindexSharedState         │
│  Fixed array[1024] of AutoindexEntry         │
│  { dboid, reloid, attno,                     │
│    scan_count, index_triggered, in_use }     │
│  Protected by one LWLock                     │
└─────────────┬────────────────────────────────┘
              │  on_shmem_exit → flush to file
              │  shmem_init   → load from file
              ▼
   $PGDATA/global/pg_autoindex.stat   (binary flat file)
              │
              │  polled every 30 seconds
              ▼
┌──────────────────────────────────────────────┐
│  AutoindexWorkerMain (background worker)     │
│  Connects to target database via OID         │
│  Scans shmem for entries where:              │
│    scan_count * COST_PER_SCAN                │
│      > INDEX_BUILD_COST                      │
│    AND !index_triggered                      │
│  Sets index_triggered = true                 │
│  Executes: CREATE INDEX ON schema.t(col)     │
│  via SPI                                     │
└──────────────────────────────────────────────┘
```

---

## Cost Model (Placeholder)

```c
/*
 * Placeholder cost constants. These are intentionally fake to get the
 * system working end-to-end. Replace with real estimates later:
 *   COST_PER_SCAN    ~ seq_page_cost * relpages  (from pg_class)
 *   INDEX_BUILD_COST ~ random_page_cost * relpages * log(relpages)
 *
 * Current threshold: 100 qualifying scans trigger index creation.
 */
#define AUTOINDEX_COST_PER_SCAN      100.0
#define AUTOINDEX_INDEX_BUILD_COST   10000.0
```

---

## Data Structures

Defined in `src/include/autoindex/pg_autoindex.h`:

```c
#define AUTOINDEX_MAX_ENTRIES  1024
#define AUTOINDEX_STAT_FILE    "global/pg_autoindex.stat"
#define AUTOINDEX_STAT_MAGIC   0xA17010EX   /* bump on struct layout change */

typedef struct AutoindexEntry {
    Oid     dboid;
    Oid     reloid;
    int16   attno;
    bool    in_use;
    bool    index_triggered;   /* true once worker has been dispatched */
    int64   scan_count;
} AutoindexEntry;

typedef struct AutoindexSharedState {
    LWLockPadded  lock;
    int           num_entries;
    AutoindexEntry entries[AUTOINDEX_MAX_ENTRIES];
} AutoindexSharedState;
```

---

## Files

### New files

| File | Purpose |
|---|---|
| `src/include/autoindex/pg_autoindex.h` | Public declarations for all autoindex code |
| `src/backend/autoindex/pg_autoindex.c` | Shmem callbacks, `autoindex_record_scan`, `autoindex_extract_equality_attno`, persistence (save/load) |
| `src/backend/autoindex/pg_autoindex_worker.c` | `AutoindexWorkerMain` + `AutoindexRegister` |
| `src/backend/autoindex/Makefile` | Builds both `.o` files into the backend |
| `src/backend/autoindex/meson.build` | Meson equivalent |

### Modified files

| File | Change |
|---|---|
| `src/include/nodes/execnodes.h` | Add `AttrNumber ai_candidate_attno` to `SeqScanState` |
| `src/backend/executor/nodeSeqscan.c` | Call `autoindex_extract_equality_attno` in Init; call `autoindex_record_scan` in End and ReScan |
| `src/include/storage/subsystemlist.h` | Add `PG_SHMEM_SUBSYSTEM(AutoindexShmemCallbacks)` near the bottom |
| `src/backend/postmaster/postmaster.c` | Call `AutoindexRegister()` alongside `ApplyLauncherRegister()` (~line 923) |
| `src/backend/Makefile` | Add `autoindex` to `SUBDIRS` |
| `src/backend/meson.build` | Add `subdir('autoindex')` |

---

## Implementation Detail

### `autoindex_extract_equality_attno(quals, scanrelid, rel)`

Called in `ExecInitSeqScan` after the relation is open. Returns the `attno` of
the first trackable column, or `0` if none found.

Logic:
1. Walk the `quals` list (these are `Node*` from `node->scan.plan.qual`).
2. For each `OpExpr` with exactly 2 args:
   - One arg must be a `Var` with `varno == scanrelid` and `varlevelsup == 0`.
   - The other arg must be a `Const`.
   - `op_hashjoinable(opexpr->opno, var->vartype)` must be true
     (reliable proxy for equality operators on standard types).
3. Check `RelationGetIndexList(rel)` — if any existing index already covers
   `var->varattno` (check `rd_index->indkey.values[i]` for each key column),
   return `0`. We do not track columns that already have an index.
4. Return `var->varattno` of the first match.

Takes the first matching column only (single-column focus for now).

### `autoindex_record_scan(dboid, reloid, attno)`

Called in `ExecEndSeqScan` and `ExecReScanSeqScan`.

Logic:
1. `LWLockAcquire(exclusive)`.
2. Linear search `entries[]` for matching `(dboid, reloid, attno)`.
3. If found: `scan_count++`.
4. If not found and space available: insert new entry with `scan_count = 1`.
5. If table full: emit `ereport(WARNING)` once, drop the entry.
6. `LWLockRelease()`.
7. `ereport(DEBUG1)` — log the updated count (useful for verifying behavior).

### `AutoindexWorkerMain`

Registered via `AutoindexRegister()` called from `postmaster.c`.

```
BackgroundWorkerInitializeConnectionByOid(target_dboid, InvalidOid, 0)

loop:
  WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH, 30000)
  ResetLatch(MyLatch)
  check for shutdown signal

  LWLockAcquire(exclusive)
  for each entry where scan_count * COST_PER_SCAN > INDEX_BUILD_COST
                   AND !index_triggered:
    mark entry->index_triggered = true
    copy (reloid, attno) locally
  LWLockRelease()

  for each copied (reloid, attno):
    /* double-check: index may have been created externally since last check */
    if index already exists on column: skip
    relname    = get_rel_name(reloid)
    schemaname = get_namespace_name(get_rel_namespace(reloid))
    colname    = get_attname(reloid, attno, false)
    SPI_connect()
    SPI_execute("CREATE INDEX ON schema.table (column)", ...)
    SPI_finish()
    ereport(LOG, "pg_autoindex: created index on ...")
```

Note: `CREATE INDEX` without `CONCURRENTLY` is used because SPI runs inside a
transaction and `CONCURRENTLY` cannot run in a transaction block. This takes a
brief `ShareLock` on the table. See Limitations.

### Persistence

**`autoindex_save_stats`** — registered via `on_shmem_exit` in `autoindex_shmem_init`:
- Write magic number, `num_entries`, and all `in_use` entries to
  `$PGDATA/global/pg_autoindex.stat` (write to `.tmp` first, then rename).

**`autoindex_load_stats`** — called at the end of `autoindex_shmem_init`:
- Read the file. On magic mismatch or I/O error: log a warning, start fresh.
- Merge loaded entries into the zero-initialized shmem array.

### Shmem Registration

`subsystemlist.h` is the canonical list of all subsystems that need shared memory.
Adding one line there causes `RegisterBuiltinShmemCallbacks()` (called in
`postmaster.c`) to invoke our `request` and `init` callbacks at the right time:

```c
/* in autoindex_shmem_request */
RequestAddinShmemSpace(sizeof(AutoindexSharedState));
RequestNamedLWLockTranche("AutoIndex", 1);

/* in autoindex_shmem_init */
AutoindexShmem = ShmemInitStruct("AutoIndex",
                                  sizeof(AutoindexSharedState), &found);
if (!found) {
    memset(AutoindexShmem, 0, sizeof(AutoindexSharedState));
    LWLockInitialize(&AutoindexShmem->lock.lock, ...);
}
autoindex_load_stats();
on_shmem_exit(autoindex_save_stats, (Datum) 0);
```

---

## Build-Phase Order

Build and test incrementally. Each phase should compile and the server should
start cleanly before moving to the next.

| Phase | Scope | Verification |
|---|---|---|
| **1** | New directory + header + empty `.c` stubs + shmem registration | Server starts; `SELECT * FROM pg_shmem_allocations WHERE name = 'AutoIndex';` returns a row |
| **2** | `autoindex_extract_equality_attno` + field in `SeqScanState` + calls in `nodeSeqscan.c` | `SET log_min_messages = DEBUG1;` then run `SELECT * FROM t WHERE col = 5` — DEBUG1 lines show incrementing scan count |
| **3** | Persistence (save/load binary file) | Increment counts, restart server, rerun a query, verify count continues from where it left off |
| **4** | `AutoindexWorkerMain` stub (logs threshold crossings, no index creation yet) | Worker appears in `SELECT * FROM pg_stat_activity;`; after enough scans, LOG line appears |
| **5** | SPI `CREATE INDEX` in the worker | After threshold, `\d tablename` shows the new index |
| **6** | End-to-end test | Fresh table, run query in loop, index appears automatically |

---

## Limitations (Known, To Address Later)

1. **No selectivity filter**: low-cardinality columns (e.g. `gender = 'M'`) will be
   indexed even if the index would be useless. Fix: read `stadistinct` from
   `pg_statistic` and skip columns below a cardinality threshold.

2. **Single database only**: the background worker connects to one database. Scans
   in other databases are tracked in shmem but never acted on.
   Fix: adopt the autovacuum launcher + per-DB worker pattern.

3. **`Var = Const` only**: join predicates (`Var = Var`) are not detected.

4. **Single-column indexes only**: composite index candidates not considered.

5. **`CREATE INDEX` without `CONCURRENTLY`**: takes a brief `ShareLock` on the
   table. Cannot use `CONCURRENTLY` inside a SPI transaction.
   Fix: use `ProcessUtility` with a special execution context, or exec a subprocess.

6. **Fixed shmem table**: overflow beyond 1024 entries is silently dropped after
   a WARNING log. Fix: increase limit or use a dynamic hash table.

7. **Cost model is fake**: `COST_PER_SCAN` and `INDEX_BUILD_COST` are constants
   unrelated to actual table size. Fix: read `relpages` from `pg_class` and
   incorporate `seq_page_cost` / `random_page_cost` GUCs.
