  #include "postgres.h"
  #include "autoindex/pg_autoindex.h"
  #include "storage/shmem.h"
  #include "storage/lwlock.h"
  #include "storage/subsystems.h"
  #include "miscadmin.h"

  AutoindexSharedState *AutoindexShmem = NULL;

  static void autoindex_shmem_request(void *arg);
  static void autoindex_shmem_init(void *arg);

  const ShmemCallbacks AutoindexShmemCallbacks = {
      .request_fn = autoindex_shmem_request,
      .init_fn    = autoindex_shmem_init,
  };

  static void
  autoindex_shmem_request(void *arg)
  {
      ShmemRequestStruct(.name = "AutoIndex",
                         .size = sizeof(AutoindexSharedState),
                         .ptr  = (void **) &AutoindexShmem);
  }

  static void
  autoindex_shmem_init(void *arg)
  {
      int tranche_id = LWLockNewTrancheId("AutoIndex");

      LWLockInitialize(&AutoindexShmem->lock.lock, tranche_id);
      AutoindexShmem->num_entries = 0;
      memset(AutoindexShmem->entries, 0, sizeof(AutoindexShmem->entries));
  }

  void
  autoindex_record_scan(Oid dboid, Oid reloid, int16 attno)
  {
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

      if (!entry->index_triggered)
          entry->scan_count++;

      LWLockRelease(&AutoindexShmem->lock.lock);

      ereport(DEBUG1,
              (errmsg("autoindex: db=%u rel=%u att=%d count=%ld",
                      dboid, reloid, (int) attno, (long) entry->scan_count)));
  }