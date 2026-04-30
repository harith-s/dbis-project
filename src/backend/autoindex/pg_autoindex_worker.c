#include "postgres.h"
#include "autoindex/pg_autoindex.h"
#include "postmaster/bgworker.h" // this header has everything you need for a bgworker, clearly
#include "storage/ipc.h"
#include "storage/latch.h"       // Latch is just for how the worker sleeps efficiently when its not doing something
#include "storage/lwlock.h"
#include "miscadmin.h"
#include "utils/lsyscache.h"     // for catalog lookups 
#include "utils/builtins.h"      // quote identifier "order" instead of order
#include "executor/spi.h"        // The server programming interface, lets backend C execute SQL strings inside a transaction
#include "utils/snapmgr.h"       // Snapshot manager, snapshot is a consistent view of database 
#include "access/xact.h"         // For bg workers to manager transactions
#include "postmaster/interrupt.h"// Provides ConfigReloadPending, SignalHandlerForConfigReload. The flag is set to true by the signal handler when postmaster sends SIGHUP (config reload).
#include "pgstat.h" 
#include "utils/guc.h"

typedef struct {
    Oid   relid;
    int16 attnums[AUTOINDEX_MAX_COLS];
    int   ncolumns;
} WorkOrder;

void AutoindexWorkerMain(Datum main_arg);
void AutoindexRegister(void);

void DropindexWorkerMain(Datum main_arg);
void DropindexRegister(void);

static volatile sig_atomic_t got_sigterm = false;

/* Signal handler for SIGTERM */
static void
autoindex_sigterm(SIGNAL_ARGS)
{
    int save_errno = errno;
    got_sigterm = true;
    SetLatch(MyLatch);
    errno = save_errno;
}

static void
dropindex_sigterm(SIGNAL_ARGS)
{
    int save_errno = errno;
    got_sigterm = true;
    SetLatch(MyLatch);
    errno = save_errno;
}


void
AutoindexWorkerMain(Datum main_arg)
{
    pqsignal(SIGHUP, SignalHandlerForConfigReload);
    pqsignal(SIGTERM, autoindex_sigterm);

    BackgroundWorkerUnblockSignals();

    BackgroundWorkerInitializeConnection("postgres", NULL, 0);

    ereport(LOG, (errmsg("autoindex worker started")));

    for (;;)
    {
        int   rc;
        
        WorkOrder candidates[AUTOINDEX_MAX_ENTRIES];
        int   ncandidates = 0;
        int   i;

        if (got_sigterm)
            proc_exit(0);

        rc = WaitLatch(MyLatch,
                       WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
                       30000L,
                       PG_WAIT_EXTENSION);
        ResetLatch(MyLatch);
        

        if (rc & WL_EXIT_ON_PM_DEATH)
            proc_exit(0);

        if (ConfigReloadPending)
        {
            ConfigReloadPending = false;
            ProcessConfigFile(PGC_SIGHUP);
        }

        if (got_sigterm)
            proc_exit(0);

        /* collect candidates under lock, mark them triggered */
        if (!AutoindexShmem)
        {
            ereport(FATAL,
                    (errmsg("autoindex worker: shared memory not initialized")));
        }
        LWLockAcquire(&AutoindexShmem->lock.lock, LW_EXCLUSIVE);
        for (i = 0; i < AutoindexShmem->num_entries; i++)
        {
            AutoindexEntry *e = &AutoindexShmem->entries[i];
            if (e->in_use &&
                !e->index_triggered &&
                e->dboid == MyDatabaseId &&
                e->scan_count >= AUTOINDEX_THRESHOLD)
            {
                e->index_triggered = true;
                candidates[ncandidates].relid = e->key.reloid;
                candidates[ncandidates].ncolumns = e->key.ncolumns;
                memcpy(candidates[ncandidates].attnums, e->key.attnums, e->key.ncolumns * sizeof(int16));
                ncandidates++;
            }
        }
        LWLockRelease(&AutoindexShmem->lock.lock);

        for (i = 0; i < ncandidates; i++)
        {
            if (got_sigterm)
                proc_exit(0);
            PG_TRY();
            {
                
                char *relname;
                char *schemaname;
                char *colname;
                char  sql[1024];
                char  col_list[1024] = "";
                char  col_idx_name[256] = "";
                char  idxname[NAMEDATALEN];
    
                SetCurrentStatementStartTimestamp();
                StartTransactionCommand();
                PushActiveSnapshot(GetTransactionSnapshot());
                SPI_connect();
    
                relname    = get_rel_name(candidates[i].relid);
                schemaname = get_namespace_name(
                                 get_rel_namespace(candidates[i].relid));
                colname    = get_attname(candidates[i].relid,
                                         candidates[i].attnums[0], false);
    
                /* Build the comma-separated list of column names */
                for (int j = 0; j < candidates[i].ncolumns; j++)
                {
                    char *colname = get_attname(candidates[i].relid, candidates[i].attnums[j], false);
                    if (j > 0) {
                        strlcat(col_idx_name, "_", sizeof(col_idx_name));
                        strlcat(col_list, ", ", sizeof(col_list));
                    }
                    strlcat(col_idx_name, colname, sizeof(col_idx_name));
                    strlcat(col_list, quote_identifier(colname), sizeof(col_list));
                }

                if (relname && schemaname && colname)
                {

                    snprintf(idxname, sizeof(idxname), "auto_idx_%u_%s", 
                        candidates[i].relid, col_idx_name);
    
                    snprintf(sql, sizeof(sql),
                        "CREATE INDEX IF NOT EXISTS %s ON %s.%s (%s)",
                        quote_identifier(idxname),
                        quote_identifier(schemaname),
                        quote_identifier(relname),
                        col_list);
    
                    SPI_execute(sql, false, 0);
    
                    ereport(LOG,
                            (errmsg("autoindex: created index on %s.%s(%s)",
                                    schemaname, relname, col_list)));
                }
    
                SPI_finish();
                PopActiveSnapshot();
                CommitTransactionCommand();
            }
            PG_CATCH();
            {
                EmitErrorReport();
                FlushErrorState();

                ereport(WARNING,
                        (errmsg("autoindex: failed to create index for relation %u", 
                                candidates[i].relid)));

                AbortCurrentTransaction();
            }
            PG_END_TRY();
        }
    }
}

void
DropindexWorkerMain(Datum main_arg)
{
    pqsignal(SIGHUP, SignalHandlerForConfigReload);
    pqsignal(SIGTERM, dropindex_sigterm);

    BackgroundWorkerUnblockSignals();
    BackgroundWorkerInitializeConnection("postgres", NULL, 0);

    ereport(LOG, (errmsg("dropindex worker started")));

    for (;;)
    {
        int   rc;
        Oid   candidates_rel[DROPINDEX_MAX_ENTRIES];
        int   ncandidates = 0;

        if (got_sigterm)
            proc_exit(0);

        rc = WaitLatch(MyLatch,
                       WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
                       30000L,
                       PG_WAIT_EXTENSION);
        ResetLatch(MyLatch);

        if (rc & WL_EXIT_ON_PM_DEATH)
            proc_exit(0);

        if (ConfigReloadPending)
        {
            ConfigReloadPending = false;
            ProcessConfigFile(PGC_SIGHUP);
        }

        if (got_sigterm)
            proc_exit(0);

        /* collect candidate relations */
        if (!DropindexShmem)
        {
            ereport(FATAL,
                    (errmsg("dropindex worker: shared memory not initialized")));
        }

        LWLockAcquire(&DropindexShmem->lock.lock, LW_EXCLUSIVE);

        for (int i = 0; i < DropindexShmem->num_entries; i++)
        {
            DropindexEntry *e = &DropindexShmem->entries[i];

            if (e->in_use &&
                !e->index_dropped &&
                e->dboid == MyDatabaseId &&
                e->update_count >= DROPINDEX_THRESHOLD)
            {
                e->index_dropped = true;
                candidates_rel[ncandidates++] = e->reloid;
            }
        }

        LWLockRelease(&DropindexShmem->lock.lock);

        /* process each relation */
        for (int i = 0; i < ncandidates; i++)
        {
            if (got_sigterm)
                proc_exit(0);

            PG_TRY();
            {
                char sql[1024];

                SetCurrentStatementStartTimestamp();
                StartTransactionCommand();
                PushActiveSnapshot(GetTransactionSnapshot());
                SPI_connect();

                /*
                 * Fetch all indexes on this table with schema
                 */
                snprintf(sql, sizeof(sql),
                         "SELECT n.nspname, c.relname "
                         "FROM pg_index i "
                         "JOIN pg_class c ON c.oid = i.indexrelid "
                         "JOIN pg_namespace n ON n.oid = c.relnamespace "
                         "WHERE i.indrelid = %u",
                         candidates_rel[i]);

                SPI_execute(sql, true, 0);

                /* ---- DROP ALL AUTO INDEXES ---- */
                for (uint64 j = 0; j < SPI_processed; j++)
                {
                    char *schemaname = SPI_getvalue(SPI_tuptable->vals[j],
                                                    SPI_tuptable->tupdesc, 1);
                    char *idxname = SPI_getvalue(SPI_tuptable->vals[j],
                                                 SPI_tuptable->tupdesc, 2);

                    if (!schemaname || !idxname)
                        continue;

                    /* only drop auto-created indexes */
                    if (strncmp(idxname, "auto_idx_", 9) != 0)
                        continue;

                    char dropsql[512];
                    snprintf(dropsql, sizeof(dropsql),
                             "DROP INDEX IF EXISTS %s.%s",
                             quote_identifier(schemaname),
                             quote_identifier(idxname));

                    SPI_execute(dropsql, false, 0);

                    ereport(LOG,
                            (errmsg("dropindex: dropped index %s.%s",
                                    schemaname, idxname)));
                }

                /* ---- RESET AUTOINDEX SHMEM ---- */
                if (AutoindexShmem)
                {
                    LWLockAcquire(&AutoindexShmem->lock.lock, LW_EXCLUSIVE);

                    for (int k = 0; k < AutoindexShmem->num_entries; k++)
                    {
                        AutoindexEntry *ae = &AutoindexShmem->entries[k];

                        if (ae->in_use &&
                            ae->dboid == MyDatabaseId &&
                            ae->key.reloid == candidates_rel[i])
                        {
                            ae->scan_count = 0;
                            ae->index_triggered = false;
                        }
                    }

                    LWLockRelease(&AutoindexShmem->lock.lock);
                }

                /* ---- RESET DROPINDEX SHMEM ---- */
                if (DropindexShmem)
                {
                    LWLockAcquire(&DropindexShmem->lock.lock, LW_EXCLUSIVE);

                    for (int k = 0; k < DropindexShmem->num_entries; k++)
                    {
                        DropindexEntry *de = &DropindexShmem->entries[k];

                        if (de->in_use &&
                            de->dboid == MyDatabaseId &&
                            de->reloid == candidates_rel[i])
                        {
                            de->update_count = 0;
                            de->index_dropped = false;
                        }
                    }

                    LWLockRelease(&DropindexShmem->lock.lock);
                }

                SPI_finish();
                PopActiveSnapshot();
                CommitTransactionCommand();
            }
            PG_CATCH();
            {
                EmitErrorReport();
                FlushErrorState();

                ereport(WARNING,
                        (errmsg("dropindex: failed for relation %u",
                                candidates_rel[i])));

                AbortCurrentTransaction();
            }
            PG_END_TRY();
        }
    }
}

void
AutoindexRegister(void)
{
    BackgroundWorker worker;

    memset(&worker, 0, sizeof(worker));
    snprintf(worker.bgw_name, BGW_MAXLEN, "autoindex worker");
    snprintf(worker.bgw_type, BGW_MAXLEN, "autoindex worker");
    worker.bgw_flags        = BGWORKER_SHMEM_ACCESS |
                              BGWORKER_BACKEND_DATABASE_CONNECTION;
    worker.bgw_start_time   = BgWorkerStart_RecoveryFinished;
    worker.bgw_restart_time = 10;
    strcpy(worker.bgw_library_name, "postgres");
    strcpy(worker.bgw_function_name, "AutoindexWorkerMain");
    worker.bgw_main_arg     = (Datum) 0;

    RegisterBackgroundWorker(&worker);
}

void
DropindexRegister(void)
{
    BackgroundWorker worker;

    memset(&worker, 0, sizeof(worker));
    snprintf(worker.bgw_name, BGW_MAXLEN, "dropindex worker");
    snprintf(worker.bgw_type, BGW_MAXLEN, "dropindex worker");
    worker.bgw_flags        = BGWORKER_SHMEM_ACCESS |
                              BGWORKER_BACKEND_DATABASE_CONNECTION;
    worker.bgw_start_time   = BgWorkerStart_RecoveryFinished;
    worker.bgw_restart_time = 10;
    strcpy(worker.bgw_library_name, "postgres");
    strcpy(worker.bgw_function_name, "DropindexWorkerMain");
    worker.bgw_main_arg     = (Datum) 0;

    RegisterBackgroundWorker(&worker);
}

