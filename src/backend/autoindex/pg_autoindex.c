  #include "postgres.h"
  #include "autoindex/pg_autoindex.h"
  #include "storage/shmem.h"
  #include "storage/lwlock.h"
  #include "storage/subsystems.h"
  #include "miscadmin.h"
  #include "access/transam.h"
  #include "optimizer/cost.h"
  #include "utils/guc.h"
  #include <math.h>

  AutoindexSharedState *AutoindexShmem = NULL;
  DropindexSharedState *DropindexShmem = NULL;
  bool autoindex_enabled = false;

  static void autoindex_shmem_request(void *arg);
  static void autoindex_shmem_init(void *arg);

  static void dropindex_shmem_request(void *arg);
  static void dropindex_shmem_init(void *arg);


  const ShmemCallbacks AutoindexShmemCallbacks = {
      .request_fn = autoindex_shmem_request,
      .init_fn    = autoindex_shmem_init,
  };

    const ShmemCallbacks DropindexShmemCallbacks = {
        .request_fn = dropindex_shmem_request,
        .init_fn    = dropindex_shmem_init,
    };

  static void
  autoindex_shmem_request(void *arg)
  {
      ShmemRequestStruct(.name = "AutoIndex",
                         .size = sizeof(AutoindexSharedState),
                         .ptr  = (void **) &AutoindexShmem);
  }

  static void 
  dropindex_shmem_request(void *arg)
  {
      ShmemRequestStruct(.name = "DropIndex",
                         .size = sizeof(DropindexSharedState),
                         .ptr  = (void **) &DropindexShmem);
  }

  static void
  autoindex_shmem_init(void *arg)
  {
      int tranche_id = LWLockNewTrancheId("AutoIndex");

      LWLockInitialize(&AutoindexShmem->lock.lock, tranche_id);
      AutoindexShmem->num_entries = 0;
      memset(AutoindexShmem->entries, 0, sizeof(AutoindexShmem->entries));
  }

  static void
  dropindex_shmem_init(void *arg)
  {
      int tranche_id = LWLockNewTrancheId("DropIndex");

      LWLockInitialize(&DropindexShmem->lock.lock, tranche_id);
      DropindexShmem->num_entries = 0;
      memset(DropindexShmem->entries, 0, sizeof(DropindexShmem->entries));
  }

  void
  autoindex_record_scan(Oid dboid, Oid reloid, int16 attno, Cost scan_cost, double build_cost)
  {
      if (reloid < FirstNormalObjectId)
          return;
      int i;
      AutoindexEntry *entry = NULL;

      if (!AutoindexShmem)
          return;

      LWLockAcquire(&AutoindexShmem->lock.lock, LW_EXCLUSIVE);

      for (i = 0; i < AutoindexShmem->num_entries; i++)
      {
          AutoindexEntry *e = &AutoindexShmem->entries[i];
          if (e->in_use &&
              e->dboid  == dboid &&
              e->reloid == reloid &&
              e->attno  == attno)
          {
              entry = e;
              break;
          }
      }

      if (!entry)
      {
          if (AutoindexShmem->num_entries >= AUTOINDEX_MAX_ENTRIES)
          {
              LWLockRelease(&AutoindexShmem->lock.lock);
              ereport(WARNING,
                      (errmsg("autoindex: shmem table full, dropping entry")));
              return;
          }
          entry = &AutoindexShmem->entries[AutoindexShmem->num_entries++];
          entry->dboid           = dboid;
          entry->reloid          = reloid;
          entry->attno           = attno;
          entry->in_use          = true;
          entry->index_triggered = false;
          entry->scan_count      = 0;
      }
      entry->build_cost = build_cost; // always update to the latest build cost

      if (!entry->index_triggered){
          entry->scan_count++;
          entry->accumulated_cost += scan_cost;
      }

      LWLockRelease(&AutoindexShmem->lock.lock);

      ereport(DEBUG1,
        (errmsg("autoindex: db=%u rel=%u att=%d scan_count=%ld "
                "accumulated_cost=%.2f build_cost=%.2f",
                dboid, reloid, (int) attno,
                (long) entry->scan_count,
                entry->accumulated_cost,
                entry->build_cost)));
  }

  void dropindex_record_scan(Oid dboid, Oid reloid, BlockNumber relpages) {
    if (reloid < FirstNormalObjectId)
          return;

    if (!DropindexShmem)
        return;

    LWLockAcquire(&AutoindexShmem->lock.lock, LW_SHARED);

    for (int i = 0; i < AutoindexShmem->num_entries; i++)
    {
        AutoindexEntry *ae = &AutoindexShmem->entries[i];

        if (!ae->in_use || ae->dboid != dboid || ae->reloid != reloid)
            continue;

        if (!ae->index_triggered)
            continue;

        // this column has an active auto-index 
        int16 attno = ae->attno;

        LWLockAcquire(&DropindexShmem->lock.lock, LW_EXCLUSIVE);

        DropindexEntry *entry = NULL;
        for (int j = 0; j < DropindexShmem->num_entries; j++)
        {
            DropindexEntry *de = &DropindexShmem->entries[j];
            if (de->in_use && de->dboid == dboid &&
                de->reloid == reloid && de->attno == attno)
            {
                entry = de;
                break;
            }
        }

        if (!entry)
        {
            if (DropindexShmem->num_entries >= DROPINDEX_MAX_ENTRIES)
            {
                LWLockRelease(&DropindexShmem->lock.lock);
                ereport(WARNING,
                        (errmsg("dropindex: shmem table full")));
                continue;
            }
            entry = &DropindexShmem->entries[DropindexShmem->num_entries++];
            entry->dboid            = dboid;
            entry->reloid           = reloid;
            entry->attno            = attno;
            entry->in_use           = true;
            entry->index_dropped    = false;
            entry->maintenance_cost = 0;
        }

        if (!entry->index_dropped)
            entry->maintenance_cost += DEFAULT_RANDOM_PAGE_COST * (1 + log(Max(relpages, 1)));

        ereport(DEBUG1,
                (errmsg("dropindex: db=%u rel=%u att=%d maintenance_cost=%.2f",
                        dboid, reloid, (int) attno, entry->maintenance_cost)));

        LWLockRelease(&DropindexShmem->lock.lock);
    }

    LWLockRelease(&AutoindexShmem->lock.lock);
}

void
  autoindex_register_gucs(void)
  {
      DefineCustomBoolVariable(
          "autoindex.enabled",
          "Enable automatic index creation and dropping.",
          NULL,
          &autoindex_enabled,
          false,
          PGC_SUSET,
          0,
          NULL, NULL, NULL
      );
  } 