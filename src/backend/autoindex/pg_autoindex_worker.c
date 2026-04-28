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

void AutoindexWorkerMain(Datum main_arg);
void AutoindexRegister(void);

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
        Oid   candidates_rel[AUTOINDEX_MAX_ENTRIES];
        int16 candidates_att[AUTOINDEX_MAX_ENTRIES];
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
                candidates_rel[ncandidates] = e->reloid;
                candidates_att[ncandidates] = e->attno;
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
                char  idxname[NAMEDATALEN];
    
                SetCurrentStatementStartTimestamp();
                StartTransactionCommand();
                PushActiveSnapshot(GetTransactionSnapshot());
                SPI_connect();
    
                relname    = get_rel_name(candidates_rel[i]);
                schemaname = get_namespace_name(
                                 get_rel_namespace(candidates_rel[i]));
                colname    = get_attname(candidates_rel[i],
                                         candidates_att[i], false);
    
                if (relname && schemaname && colname)
                {
                    snprintf(idxname, sizeof(idxname), "auto_idx_%u_%d", 
                        candidates_rel[i], candidates_att[i]);
    
                    snprintf(sql, sizeof(sql),
                        "CREATE INDEX IF NOT EXISTS %s ON %s.%s (%s)",
                        quote_identifier(idxname),
                        quote_identifier(schemaname),
                        quote_identifier(relname),
                        quote_identifier(colname));
    
                    SPI_execute(sql, false, 0);
    
                    ereport(LOG,
                            (errmsg("autoindex: created index on %s.%s(%s)",
                                    schemaname, relname, colname)));
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
