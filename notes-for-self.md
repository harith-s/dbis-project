# PostgreSQL Source Code Guide: Automated Index Learning

## 1. High-Level Source Tree

The Postgres source is organized under `src/backend/`:

```
src/
├── backend/
│   ├── optimizer/       ← Query planner & cost model (most important for you)
│   ├── executor/        ← Query execution engine
│   ├── storage/         ← Buffer manager, heap, index storage
│   ├── access/          ← Index access methods (btree, hash, gist, gin…)
│   ├── commands/        ← DDL commands including CREATE INDEX
│   ├── parser/          ← SQL parsing
│   ├── tcop/            ← Traffic cop — query lifecycle entry point
│   ├── utils/           ← Statistics, catalogs, memory
│   └── statistics/      ← Extended statistics
├── include/             ← All headers
└── tools/
```

---

## 2. The Query Lifecycle (Where You'll Hook In)

```
Client SQL
    │
    ▼
parser/          → raw parse tree
    │
    ▼
analyze/         → semantic analysis (pg_analyze.c)
    │
    ▼
rewrite/         → rule rewriting
    │
    ▼
optimizer/       → planning + cost estimation  ← YOUR PRIMARY ARENA
    │
    ▼
executor/        → execution + runtime stats   ← FEEDBACK LOOP HERE
```

For automated index learning you care about **two phases**:
- **Optimizer** — to simulate "what if this index existed?"
- **Executor** — to collect actual vs. estimated row counts, actual scan costs

---

## 3. The Optimizer — Deep Dive

**`src/backend/optimizer/`** is your most important directory:

| File | Purpose |
|---|---|
| `path/costsize.c` | All cost formulas for seq scan, index scan, hash join, etc. |
| `path/indxpath.c` | **Generates index paths** — critical for you |
| `plan/planner.c` | Top-level planning entry point (`planner()`, `standard_planner()`) |
| `plan/createplan.c` | Converts paths → plan nodes |
| `geqo/` | Genetic query optimizer for large join counts |
| `prep/` | Query preprocessing |
| `util/plancat.c` | **Reads catalog info about indexes** — key for hypothetical indexes |

### `indxpath.c` — The Heart of Index Selection

This file decides which indexes are considered for a query. Key functions:

```c
create_index_paths()          // Entry: generates all IndexPath candidates
build_index_paths()           // Builds paths for one index
match_clause_to_index()       // Can a WHERE clause use this index?
match_join_clauses_to_index() // Join condition index matching
```

To implement **hypothetical indexes** (the standard approach for index recommendation), you inject fake `IndexOptInfo` structs here and let the planner cost them normally.

### `costsize.c` — Cost Model

```c
cost_index()     // Cost of an index scan
cost_seqscan()   // Cost of a sequential scan
cost_sort()      // Sorting cost (relevant for index-vs-sort decisions)
```

These are the functions you'll call when evaluating "how much would a new index help?"

### `plancat.c` — Catalog Interface

```c
get_relation_info()   // Loads table + index info into RelOptInfo
```

This is where real indexes are loaded. For hypothetical indexes, you'd intercept here or add a parallel path.

---

## 4. Index Access Methods — `src/backend/access/`

```
access/
├── brin/       ← Block Range INdexes
├── gin/        ← Generalized Inverted Index (full-text, arrays)
├── gist/       ← Generalized Search Tree
├── hash/       ← Hash indexes
├── nbtree/     ← B-tree (most common — start here)
│   ├── nbtree.c
│   ├── nbtsearch.c
│   └── nbtinsert.c
└── index/
    ├── indexam.c     ← Generic index AM interface
    └── genam.c       ← Generic index scan
```

The **index AM (access method) API** in `src/include/access/amapi.h` defines the interface all index types implement. For automated learning, you mostly care about B-tree costs, but understanding the AM API matters if you want to simulate index builds.

---

## 5. Statistics — The Foundation of Good Recommendations

**`src/backend/statistics/`** and **`src/backend/utils/adt/selfuncs.c`**:

```c
// selfuncs.c — selectivity estimation functions
eqsel()          // Equality predicate selectivity
scalarltsel()    // Range predicate selectivity
patternsel()     // LIKE pattern selectivity
```

These drive cost estimates. Your index recommendation system needs to understand which columns have high selectivity (good index candidates) vs. low selectivity (bad candidates).

**`pg_statistic` catalog** — where column stats live:

| Column | Meaning |
|---|---|
| `stadistinct` | Number of distinct values |
| `stanullfrac` | Fraction of NULLs |
| `stavalues` / `stafracs` | Most common values / frequencies |
| `histogram_bounds` | Distribution histogram |

---

## 6. Key Catalog Tables You'll Query

| Catalog | Content |
|---|---|
| `pg_index` | Existing indexes |
| `pg_class` | Tables and indexes |
| `pg_attribute` | Columns |
| `pg_statistic` | Column statistics |
| `pg_stats` | Human-readable view of pg_statistic |
| `pg_am` | Index access methods |

---

## 7. Architecture for Automated Index Learning

```
┌─────────────────────────────────────────────┐
│              Query Workload Log              │
│   (pg_stat_statements or custom hook)       │
└──────────────────┬──────────────────────────┘
                   │
                   ▼
┌─────────────────────────────────────────────┐
│           Candidate Index Generator         │
│  • Extract filter/join/orderby columns      │
│  • Consider multi-column combos             │
│  • Prune low-selectivity candidates         │
└──────────────────┬──────────────────────────┘
                   │
                   ▼
┌─────────────────────────────────────────────┐
│         What-If / Hypothetical Optimizer    │
│  • Inject fake IndexOptInfo into planner    │
│  • Run cost_index() for each candidate      │
│  • Compare plan cost with vs. without       │
└──────────────────┬──────────────────────────┘
                   │
                   ▼
┌─────────────────────────────────────────────┐
│          Benefit & Cost Analysis            │
│  • Index build cost (pg_relation_size)      │
│  • Maintenance overhead (write slowdown)    │
│  • Net benefit across workload              │
└──────────────────┬──────────────────────────┘
                   │
                   ▼
┌─────────────────────────────────────────────┐
│         Recommendation / Auto-Apply         │
│  • Output DDL or auto CREATE INDEX          │
└─────────────────────────────────────────────┘
```

---

## 8. The HypoPG Extension — Best Starting Reference

**HypoPG** (`github.com/HypoPG/hypopg`) is the best existing reference implementation. It works by:

1. Adding fake index entries to `pg_index` visible only within a session
2. Hooking into `get_relation_info()` via the `get_relation_info_hook`
3. Letting the real planner cost those fake indexes normally

Key hook in Postgres for this:

```c
// src/include/optimizer/plancat.h
extern PGDLLIMPORT get_relation_info_hook_type get_relation_info_hook;
```

This is **the primary hook** you'll use.

---

## 9. Extension Points & Hooks Relevant to You

```c
// Query execution feedback
ExecutorEnd_hook          // Fires after query — capture actual rows/time
ExecutorRun_hook          // During execution

// Planning
get_relation_info_hook    // Inject hypothetical indexes ← most important
planner_hook              // Intercept planning

// Statement tracking
pg_stat_statements        // Built-in workload logging (enable this)
```

---

## 10. Recommended Reading Order in the Source

1. **`src/backend/optimizer/plan/planner.c`** — understand `standard_planner()`
2. **`src/backend/optimizer/path/indxpath.c`** — how indexes become paths
3. **`src/backend/optimizer/path/costsize.c`** — `cost_index()` specifically
4. **`src/backend/optimizer/util/plancat.c`** — `get_relation_info()` and the hook
5. **`src/backend/utils/adt/selfuncs.c`** — selectivity estimation
6. **`src/include/nodes/pathnodes.h`** — `IndexOptInfo`, `RelOptInfo`, `Path` structs
7. **`src/include/access/amapi.h`** — index AM interface

---

## 11. Implementation Roadmap

### Phase 1 — Workload Capture
- Enable `pg_stat_statements`
- Hook `ExecutorEnd_hook` to capture query text, actual rows, actual time
- Build a workload table: query fingerprint → frequency, total cost

### Phase 2 — Candidate Generation
- Parse captured queries for WHERE / JOIN ON / ORDER BY columns
- Score candidates by selectivity from `pg_stats`
- Generate single-column and multi-column (composite) candidates

### Phase 3 — Hypothetical Index Evaluation
- Hook `get_relation_info_hook`
- Inject `IndexOptInfo` structs for each candidate
- Re-plan each workload query and capture estimated cost delta

### Phase 4 — Benefit Analysis & Recommendation
- Compute: `benefit = Σ (cost_without - cost_with) × query_frequency`
- Compute: `build_cost ∝ table_size × num_columns`
- Compute: `maintenance_cost ∝ write_frequency × index_size`
- Rank by `net_benefit = benefit - build_cost - maintenance_cost`
- Output `CREATE INDEX` DDL for top-N candidates

### Phase 5 — Feedback Loop
- After index creation, compare actual vs. predicted cost savings
- Adjust selectivity model if estimates were off
- Drop indexes that proved unhelpful after a trial window

---

## 12. Key Structs to Understand

```c
// src/include/nodes/pathnodes.h

typedef struct IndexOptInfo {
    NodeTag     type;
    Oid         indexoid;       // OID of the index relation
    Oid         reltablespace;  // tablespace of index
    RelOptInfo *rel;            // back-link to the relation
    BlockNumber pages;          // number of disk pages in index
    double      tuples;         // number of index tuples
    int         tree_height;    // index tree height (-1 = unknown)
    int         ncolumns;       // number of index columns
    int        *indexkeys;      // column numbers of index's keys, or 0
    Oid        *indexcollations;
    Oid        *opfamily;       // OIDs of operator families for columns
    Oid        *opcintype;      // OIDs of opclass declared input data types
    bool        unique;         // true if a unique index
    bool        hypothetical;   // true if index doesn't really exist (HypoPG adds this)
    /* ... more fields ... */
} IndexOptInfo;
```

For hypothetical indexes, you allocate one of these, fill in realistic values (estimated `pages`, `tuples`, etc.), and append it to `rel->indexlist`.

---

## 13. Suggested First Steps

1. **Enable `pg_stat_statements`** — add to `postgresql.conf`:
   ```
   shared_preload_libraries = 'pg_stat_statements'
   pg_stat_statements.track = all
   ```

2. **Study HypoPG's source** — it's ~2000 lines and directly implements the hypothetical index pattern at `github.com/HypoPG/hypopg`

3. **Write a minimal hook** on `get_relation_info_hook` — inject one fake index for a known table, confirm the planner sees and prices it with `EXPLAIN`

4. **Build a candidate generator** that reads `pg_stat_statements.query` and extracts indexed column candidates

5. **Benchmark** using `EXPLAIN (ANALYZE, BUFFERS)` before and after to validate your cost model