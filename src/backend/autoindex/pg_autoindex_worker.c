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
                       5000L,
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
                e->accumulated_cost >= e->build_cost * (1 << Min(e->drop_count, 3)))
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
        int16 candidates_att[DROPINDEX_MAX_ENTRIES];

        int   ncandidates = 0;

        if (got_sigterm)
            proc_exit(0);

        rc = WaitLatch(MyLatch,
                       WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
                       5000L,
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

        /* Pass 1: collect candidates from DropindexShmem */
        LWLockAcquire(&DropindexShmem->lock.lock, LW_EXCLUSIVE);
        for (int i = 0; i < DropindexShmem->num_entries; i++)
        {
            DropindexEntry *e = &DropindexShmem->entries[i];

            if (!e->in_use || e->index_dropped || e->dboid != MyDatabaseId)
                continue;

            candidates_rel[ncandidates] = e->reloid;
            candidates_att[ncandidates] = e->attno;
            ncandidates++;
        }

        LWLockRelease(&DropindexShmem->lock.lock);

        /* Pass 2: check scan_benefit from AutoindexShmem, filter actual drop candidates */
        int ndrop = 0;
        Oid drop_rel[DROPINDEX_MAX_ENTRIES];
        int16 drop_att[DROPINDEX_MAX_ENTRIES];

        LWLockAcquire(&AutoindexShmem->lock.lock, LW_SHARED);

        for (int i = 0; i < ncandidates; i++)
        {
            for (int j = 0; j < AutoindexShmem->num_entries; j++)
            {
                AutoindexEntry *ae = &AutoindexShmem->entries[j];
                if (ae->in_use && ae->dboid == MyDatabaseId &&
                    ae->reloid == candidates_rel[i] && ae->attno == candidates_att[i])
                {
                    LWLockAcquire(&DropindexShmem->lock.lock, LW_SHARED);
                    for (int k = 0; k < DropindexShmem->num_entries; k++)
                    {
                        DropindexEntry *de = &DropindexShmem->entries[k];
                        if (de->in_use && de->dboid == MyDatabaseId &&
                            de->reloid == candidates_rel[i] && de->attno == candidates_att[i])
                        {
                            if (de->maintenance_cost >= ae->accumulated_cost && ae->accumulated_cost > 0)
                            {
                                drop_rel[ndrop] = candidates_rel[i];
                                drop_att[ndrop] = candidates_att[i];
                                ndrop++;
                            }
                            break;
                        }
                    }
                    LWLockRelease(&DropindexShmem->lock.lock);
                    break;
                }
            }
        }
        LWLockRelease(&AutoindexShmem->lock.lock);
        
        /* mark confirmed drops in DropindexShmem */
        LWLockAcquire(&DropindexShmem->lock.lock, LW_EXCLUSIVE);
        for (int i = 0; i < ndrop; i++)
        {
            for (int j = 0; j < DropindexShmem->num_entries; j++)
            {
                DropindexEntry *de = &DropindexShmem->entries[j];
                if (de->in_use && de->dboid == MyDatabaseId &&
                    de->reloid == drop_rel[i] && de->attno == drop_att[i])
                {
                    de->index_dropped = true;
                    break;
                }
            }
        }
        LWLockRelease(&DropindexShmem->lock.lock);

        for (int i = 0; i < ndrop; i++)
        {
            if (got_sigterm)
                proc_exit(0);

            PG_TRY();
            {
                char sql[512];
                char idxname[NAMEDATALEN];
                char *schemaname;
                char *relname;

                SetCurrentStatementStartTimestamp();
                StartTransactionCommand();
                PushActiveSnapshot(GetTransactionSnapshot());
                SPI_connect();

                relname    = get_rel_name(drop_rel[i]);
                schemaname = get_namespace_name(get_rel_namespace(drop_rel[i]));

                snprintf(idxname, sizeof(idxname), "auto_idx_%u_%d",
                        drop_rel[i], drop_att[i]);

                if (relname && schemaname)
                {   
                    snprintf(sql, sizeof(sql),
                            "DROP INDEX IF EXISTS %s.%s",
                            quote_identifier(schemaname),
                            quote_identifier(idxname));

                    SPI_execute(sql, false, 0);

                    ereport(LOG,
                            (errmsg("dropindex: dropped index %s.%s",
                                    schemaname, idxname)));
                }

                
                if (AutoindexShmem)
                {
                    LWLockAcquire(&AutoindexShmem->lock.lock, LW_EXCLUSIVE);
                    for (int k = 0; k < AutoindexShmem->num_entries; k++)
                    {
                        AutoindexEntry *ae = &AutoindexShmem->entries[k];
                        if (ae->in_use && ae->dboid == MyDatabaseId &&
                            ae->reloid == drop_rel[i] && ae->attno == drop_att[i])

                        if (ae->in_use &&
                            ae->dboid == MyDatabaseId &&
                            ae->key.reloid == candidates_rel[i])
                        {
                            ae->drop_count++;
                            ae->scan_count = 0;
                            ae->accumulated_cost = 0;
                            ae->index_triggered = false;
                            break;
                        }
                    }
                    LWLockRelease(&AutoindexShmem->lock.lock);
                }

                if (DropindexShmem)
                {
                    LWLockAcquire(&DropindexShmem->lock.lock, LW_EXCLUSIVE);
                    for (int k = 0; k < DropindexShmem->num_entries; k++)
                    {
                        DropindexEntry *de = &DropindexShmem->entries[k];
                        if (de->in_use && de->dboid == MyDatabaseId &&
                            de->reloid == drop_rel[i] && de->attno == drop_att[i])
                        {
                            de->maintenance_cost = 0;
                            de->index_dropped = false;
                            break;
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
                        (errmsg("dropindex: failed for relation %u att %d",
                                drop_rel[i], drop_att[i])));
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

