#!/usr/bin/env python3
"""
pg_autoindex Performance Benchmark
===================================
Demonstrates the performance improvement from the automatic index creation
feature built into this custom PostgreSQL fork.

The autoindex background worker monitors sequential scans with equality
predicates. When the accumulated scan cost exceeds the estimated index build
cost it automatically issues CREATE INDEX. This script measures query
execution time before and after that happens.

Usage:
    # Make sure the custom postgres is running on port 5433 first:
    #   ~/postgres_custom/bin/pg_ctl -D ~/custom_data -o "-p 5433" start
    python3 pg_autoindex/test_performance.py
"""

import subprocess
import time
import re
import os
import sys
import statistics

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

PSQL_BIN = os.path.expanduser("~/postgres_custom/bin/psql")
PSQL_ARGS = [PSQL_BIN, "-p", "5433", "-d", "postgres", "-t", "-A"]

N_WARMUP   = 3    # Throwaway runs before measuring baseline
N_MEASURED = 7    # Runs to average for before/after timing
N_TRIGGER  = 25   # Extra trigger scans (on top of warmup + baseline)
WAIT_SECS  = 90   # Max seconds to wait for background worker to create index


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def psql(sql: str, quiet: bool = False) -> str:
    """Run SQL in a new psql session and return stdout."""
    result = subprocess.run(
        PSQL_ARGS + ["-c", sql],
        capture_output=True, text=True
    )
    if result.returncode != 0 and not quiet:
        print(f"  [psql error] {result.stderr.strip()}")
    return result.stdout


def explain_time_ms(sql: str) -> float | None:
    """
    Run EXPLAIN (ANALYZE, TIMING ON) and return Execution Time in ms.
    autoindex.enabled is set within the same session so scan recording fires.
    """
    combined = (
        "SET autoindex.enabled = true; "
        "SET max_parallel_workers_per_gather = 0; "
        f"EXPLAIN (ANALYZE, TIMING ON, FORMAT TEXT) {sql}"
    )
    out = psql(combined, quiet=True)
    m = re.search(r"Execution Time:\s*([\d.]+)\s*ms", out)
    return float(m.group(1)) if m else None


def avg_exec_ms(sql: str, n: int = N_MEASURED) -> tuple[float, float]:
    """Return (mean_ms, stdev_ms) over n EXPLAIN ANALYZE runs."""
    times = []
    for _ in range(n):
        t = explain_time_ms(sql)
        if t is not None:
            times.append(t)
    if not times:
        return 0.0, 0.0
    mean = statistics.mean(times)
    stdev = statistics.stdev(times) if len(times) > 1 else 0.0
    return mean, stdev


def get_query_plan(sql: str) -> str:
    """Return the first two lines of EXPLAIN output (plan node summary)."""
    combined = (
        "SET autoindex.enabled = true; "
        "SET max_parallel_workers_per_gather = 0; "
        f"EXPLAIN (FORMAT TEXT) {sql}"
    )
    lines = [l for l in psql(combined, quiet=True).splitlines() if l.strip()]
    return "\n".join(lines[:3]) if lines else "(no plan)"


def trigger_scans(sql: str, count: int):
    """
    Run `sql` count times to accumulate scan cost in shared memory.
    Uses a single psql call per batch to minimize overhead.
    """
    combined = (
        "SET autoindex.enabled = true; "
        "SET max_parallel_workers_per_gather = 0; "
        + (sql + "; ") * count
    )
    psql(combined, quiet=True)


def wait_for_index(table: str, column: str, timeout: int = WAIT_SECS) -> bool:
    """
    Poll pg_indexes until an auto-created index on table(column) appears,
    or until timeout expires. Returns True if found.
    """
    check = (
        f"SELECT COUNT(*) FROM pg_indexes "
        f"WHERE tablename = '{table}' "
        f"AND indexdef LIKE '% ({column})%' "
        f"AND indexname LIKE 'auto_idx%'"
    )
    deadline = time.time() + timeout
    while time.time() < deadline:
        out = psql(check, quiet=True).strip()
        if out == "1":
            return True
        time.sleep(3)
    return False


def drop_autoindex(table: str, column: str):
    """Drop any auto-created index on table(column) for a clean slate."""
    out = psql(
        f"SELECT indexname FROM pg_indexes "
        f"WHERE tablename = '{table}' "
        f"AND indexdef LIKE '% ({column})%' "
        f"AND indexname LIKE 'auto_idx%'",
        quiet=True
    ).strip()
    if out:
        psql(f"DROP INDEX IF EXISTS {out}", quiet=True)


def sep(char="─", width=70):
    print(char * width)


def pct_improvement(before: float, after: float) -> float:
    if before == 0:
        return 0.0
    return (before - after) / before * 100.0


# ---------------------------------------------------------------------------
# Test runner
# ---------------------------------------------------------------------------

def run_test(
    title: str,
    setup_sql: str,
    query: str,
    table: str,
    column: str,
    why: str,
):
    sep("═")
    print(f"  TEST: {title}")
    sep("═")

    # --- Setup ---------------------------------------------------------------
    print("\n  Setting up table...")
    psql(setup_sql, quiet=False)
    drop_autoindex(table, column)
    time.sleep(1)

    # --- Warmup (uncounted) --------------------------------------------------
    print(f"  Warming up ({N_WARMUP} throwaway runs)...", end="", flush=True)
    for _ in range(N_WARMUP):
        explain_time_ms(query)
        print(".", end="", flush=True)
    print()

    # --- Baseline (before index) ---------------------------------------------
    print(f"  Measuring BEFORE (autoindex disabled for first pass)...")
    before_plan = get_query_plan(query)
    before_mean, before_sd = avg_exec_ms(query, N_MEASURED)
    print(f"    Plan: {before_plan.splitlines()[0].strip()}")
    print(f"    Avg execution time: {before_mean:.2f} ms  (±{before_sd:.2f} ms, n={N_MEASURED})")

    # --- Trigger scans -------------------------------------------------------
    # N_WARMUP + N_MEASURED runs already accumulated cost; add N_TRIGGER more.
    print(f"\n  Triggering autoindex ({N_TRIGGER} additional scans)...", end="", flush=True)
    trigger_scans(query, N_TRIGGER)
    print(" done.")

    # --- Wait for background worker ------------------------------------------
    print(f"  Waiting for background worker to create index on {table}({column})...", end="", flush=True)
    found = wait_for_index(table, column, WAIT_SECS)
    if not found:
        print(f"\n  ✗ Index did not appear within {WAIT_SECS}s. Skipping after-measurement.")
        return
    print(" created!")

    # Give the planner a chance to see the new index via cache invalidation
    time.sleep(1)

    # --- After index ---------------------------------------------------------
    print(f"  Measuring AFTER (index now in place)...")
    after_plan = get_query_plan(query)
    after_mean, after_sd = avg_exec_ms(query, N_MEASURED)
    print(f"    Plan: {after_plan.splitlines()[0].strip()}")
    print(f"    Avg execution time: {after_mean:.2f} ms  (±{after_sd:.2f} ms, n={N_MEASURED})")

    # --- Summary -------------------------------------------------------------
    pct = pct_improvement(before_mean, after_mean)
    speedup = before_mean / after_mean if after_mean > 0 else float("inf")

    sep()
    print(f"  RESULTS — {title}")
    sep()
    print(f"    Before (SeqScan) : {before_mean:8.2f} ms")
    print(f"    After  (IdxScan) : {after_mean:8.2f} ms")
    print(f"    Improvement      : {pct:7.1f}%  ({speedup:.1f}× faster)")
    print()
    print(f"  WHY THIS MATTERS:")
    for line in why.strip().splitlines():
        print(f"    {line}")
    print()


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    # Verify psql binary exists
    if not os.path.exists(PSQL_BIN):
        print(f"ERROR: psql not found at {PSQL_BIN}")
        print("Build and install the custom postgres first (see HowToCheck.md).")
        sys.exit(1)

    sep("═")
    print("  pg_autoindex — Performance Benchmark Suite")
    sep("═")
    print(f"  Connecting via: {PSQL_BIN} -p 5433")
    print(f"  Settings: {N_MEASURED} measured runs, {N_TRIGGER} trigger scans, "
          f"{WAIT_SECS}s index wait")

    # Enable autoindex globally so it survives across psql sessions
    psql("ALTER SYSTEM SET autoindex.enabled = true", quiet=True)
    psql("SELECT pg_reload_conf()", quiet=True)
    time.sleep(1)

    # =========================================================================
    # Test 1 — Point Lookup (equality scan, no joins)
    #
    # Why: The most common OLTP query pattern. A table with 200 K orders is
    # repeatedly filtered by customer_id. Without an index the planner must
    # scan every row; with one it descends a B-tree in O(log N) steps and
    # fetches only the matching heap pages.
    # =========================================================================
    run_test(
        title="Point Lookup — orders by customer_id",
        setup_sql="""
            DROP TABLE IF EXISTS orders CASCADE;
            CREATE TABLE orders (
                order_id    SERIAL PRIMARY KEY,
                customer_id INT    NOT NULL,
                amount      NUMERIC(10,2) NOT NULL,
                status      TEXT   NOT NULL,
                order_date  DATE   NOT NULL
            );
            INSERT INTO orders (customer_id, amount, status, order_date)
            SELECT
                (g % 5000) + 1,
                ROUND((random() * 900 + 100)::numeric, 2),
                CASE (g % 4)
                    WHEN 0 THEN 'pending'
                    WHEN 1 THEN 'shipped'
                    WHEN 2 THEN 'delivered'
                    ELSE        'cancelled'
                END,
                DATE '2022-01-01' + (g % 730)
            FROM generate_series(1, 200000) g;
            ANALYZE orders;
        """,
        query="SELECT order_id, amount, status FROM orders WHERE customer_id = 500",
        table="orders",
        column="customer_id",
        why="""\
The orders table has 200 K rows spread across ~5 K customers (~40 rows/customer).
Without an index the database reads every row (sequential scan).
With the auto-created B-tree index on customer_id it reads only 40 rows via
an index scan — roughly a 5 000× reduction in rows examined, which translates
directly into the execution-time speedup shown above.""",
    )

    # =========================================================================
    # Test 2 — Aggregation: GROUP BY + ORDER BY
    #
    # Why: A common analytics pattern. The WHERE filter on product_id must be
    # satisfied before aggregation. Without an index the planner fetches all
    # rows then discards non-matching ones; with an index it fetches only the
    # matching rows up front, making the GROUP BY / ORDER BY work on a much
    # smaller input.
    # =========================================================================
    run_test(
        title="Aggregation — sales per region for a product (GROUP BY + ORDER BY)",
        setup_sql="""
            DROP TABLE IF EXISTS sales CASCADE;
            CREATE TABLE sales (
                sale_id    SERIAL PRIMARY KEY,
                product_id INT    NOT NULL,
                region_id  INT    NOT NULL,
                amount     NUMERIC(10,2) NOT NULL,
                quantity   INT    NOT NULL,
                sale_date  DATE   NOT NULL
            );
            INSERT INTO sales (product_id, region_id, amount, quantity, sale_date)
            SELECT
                (g % 2000) + 1,
                (g % 20) + 1,
                ROUND((random() * 500 + 50)::numeric, 2),
                (g % 10) + 1,
                DATE '2021-01-01' + (g % 1095)
            FROM generate_series(1, 300000) g;
            ANALYZE sales;
        """,
        query=(
            "SELECT region_id, COUNT(*) AS num_sales, "
            "SUM(amount) AS total_revenue, ROUND(AVG(quantity)::numeric, 1) AS avg_qty "
            "FROM sales WHERE product_id = 42 "
            "GROUP BY region_id ORDER BY total_revenue DESC"
        ),
        table="sales",
        column="product_id",
        why="""\
The sales table has 300 K rows across 2 K products (~150 rows per product).
The GROUP BY and ORDER BY operate on the already-filtered result set, so
reducing the input via an index on product_id makes every subsequent stage
(hash aggregate, sort) faster as well.
Without the index: full sequential scan → aggregate 300 K rows.
With the auto-index: index scan fetches ~150 rows → aggregate those only.""",
    )

    # =========================================================================
    # Test 3 — JOIN + WHERE + GROUP BY + ORDER BY (all four clauses)
    #
    # Why: A realistic HR analytics query. The driving table (employees) is
    # filtered by manager_id before being joined to departments. Without an
    # index on manager_id the planner scans all 150 K employees just to find
    # the ~100 who report to a given manager.  The auto-index converts that
    # into a targeted B-tree lookup, shrinking the join input dramatically.
    # =========================================================================
    run_test(
        title="JOIN + WHERE + GROUP BY + ORDER BY — employee headcount by department",
        setup_sql="""
            DROP TABLE IF EXISTS employees CASCADE;
            DROP TABLE IF EXISTS departments CASCADE;

            CREATE TABLE departments (
                dept_id   SERIAL PRIMARY KEY,
                dept_name TEXT   NOT NULL,
                budget    NUMERIC(12,2)
            );
            INSERT INTO departments (dept_name, budget)
            SELECT 'Dept_' || g, ROUND((random() * 9000000 + 1000000)::numeric, 2)
            FROM generate_series(1, 200) g;

            CREATE TABLE employees (
                emp_id     SERIAL PRIMARY KEY,
                dept_id    INT    NOT NULL REFERENCES departments(dept_id),
                manager_id INT    NOT NULL,
                salary     NUMERIC(10,2) NOT NULL,
                hire_year  INT    NOT NULL,
                name       TEXT   NOT NULL
            );
            INSERT INTO employees (dept_id, manager_id, salary, hire_year, name)
            SELECT
                (g % 200) + 1,
                (g % 1500) + 1,
                ROUND((40000 + random() * 120000)::numeric, 2),
                2005 + (g % 20),
                'Employee_' || g
            FROM generate_series(1, 150000) g;
            ANALYZE employees, departments;
        """,
        query=(
            "SELECT d.dept_name, COUNT(*) AS headcount, "
            "ROUND(AVG(e.salary)::numeric, 2) AS avg_salary, "
            "SUM(e.salary) AS total_payroll "
            "FROM employees e "
            "JOIN departments d ON e.dept_id = d.dept_id "
            "WHERE e.manager_id = 100 "
            "GROUP BY d.dept_name "
            "ORDER BY avg_salary DESC"
        ),
        table="employees",
        column="manager_id",
        why="""\
This query uses all four SQL clauses: WHERE (filter by manager), JOIN (enrich
with department names), GROUP BY (aggregate per department), ORDER BY (rank by
salary). The bottleneck without the index is scanning all 150 K employees to
find the ~100 who report to manager 100.
The auto-index on manager_id lets the planner use an Index Scan that fetches
only those ~100 rows before the join and aggregate steps, making the entire
query proportionally faster. The plan change from SeqScan → IndexScan on the
employees table is the key signal visible in EXPLAIN output.""",
    )

    # Restore autoindex.enabled to default to avoid affecting other sessions
    psql("ALTER SYSTEM RESET autoindex.enabled", quiet=True)
    psql("SELECT pg_reload_conf()", quiet=True)

    sep("═")
    print("  Benchmark complete.")
    sep("═")


if __name__ == "__main__":
    main()
