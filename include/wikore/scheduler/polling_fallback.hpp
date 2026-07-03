#pragma once
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <atomic>
#include <chrono>
#include <functional>
#include <string>

namespace wikore::scheduler {

// ---------------------------------------------------------------------------
// PollingFallback - reliability backstop for the ingest pipeline.
//
// Sweeps three failure modes per cycle:
//
//   1. document_versions.ingest_status='pending' beyond `stuck_threshold`
//      (predicate is updated_at, not created_at, so a row freshly reset
//      by sweep #2 isn't immediately re-promoted by sweep #1):
//        * payload IS NULL                  -> 'error' (api crash etc.)
//        * payload IS NOT NULL + budget left -> requeue (LPUSH source +
//          increment retry); covers the LMOVE-then-crash-before-flip
//          window inside IngestWorker::dispatch
//        * payload IS NOT NULL + budget gone -> 'error'
//
//   2. document_versions.ingest_status='processing' beyond `stuck_threshold`:
//      identical recovery semantics to sweep #1's payload branch.
//
//   3. Orphaned Redis processing lists (`lr:ingest:proc:{worker_id}`)
//      whose heartbeat key (`lr:ingest:hb:{worker_id}`) has expired:
//      DB-consulted cleanup. For each entry:
//        * row terminal or DB-payload present -> LREM only
//        * row non-terminal AND payload absent -> TRANSFER back to source
//          queue (LPUSH then LREM, atomic-ish; LPUSH failure leaves
//          entry on proc list for next-cycle retry, so the job is never
//          lost)
//      Tri-state EXISTS: only 0 = dead; -1 = skip-and-retry.
//
// Sweeps 1 and 2 run inside one xact_advisory_lock so concurrent
// scheduler instances don't double-sweep. Their post-commit LPUSH has
// an in-loop retry; on terminal LPUSH failure the DB transition is
// rolled back so the row stays recoverable on the next sweep.
//
// Sweep 3 runs outside the lock because it's Redis-driven and
// self-coordinating; concurrent execution by multiple schedulers is
// safe because each transfer is LPUSH-then-LREM (idempotent: a second
// sweeper sees the entry gone).
// ---------------------------------------------------------------------------

class PollingFallback {
public:
    using ShutdownPredicate = std::function<bool()>;

    struct Options {
        std::chrono::seconds  interval         = std::chrono::minutes(1);
        std::chrono::minutes  stuck_threshold  = std::chrono::minutes(15);
        // Wake interval during the inter-sweep sleep. The sleep is chunked so a
        // SIGTERM arriving right after a sweep returns within one chunk instead
        // of blocking the whole `interval` -- keeps daemon shutdown prompt.
        std::chrono::milliseconds sleep_chunk   = std::chrono::seconds(1);
        // Test hook: invoked once each time the inter-sweep sleep chunk is armed
        // (right after the timer is scheduled), letting a test flip shutdown
        // exactly while the coroutine is suspended in the sleep. Unused in
        // production (empty).
        std::function<void()> on_sleep_armed;
        // Cap on how many times a single document_version can be requeued
        // after a worker crash. Past this, the row goes to 'error' so a
        // poison message cannot loop forever.
        int                   max_resume_attempts = 3;
        // Postgres advisory-lock keyspace name; hashtext() gives a stable
        // 32-bit key shared by every scheduler instance with the same name.
        std::string           advisory_lock_name = "wikore-scheduler-polling";
    };

    PollingFallback(drogon::orm::DbClientPtr db,
                    ShutdownPredicate        shutdown_requested,
                    Options                  opts);

    drogon::Task<void> run();

    // Drains a single sweep. Returns the total number of stuck rows
    // promoted/requeued across all three sweeps.
    drogon::Task<int>  sweep_once();

    std::size_t total_swept() const { return total_swept_.load(); }

private:
    // Result of one orphan-reap pass.
    struct ReapResult {
        int                      transferred = 0;
        // Version IDs for which the pre-transfer touch UPDATE failed
        // (DB error, not 0-rows). Sweep #1 in the same sweep_once()
        // MUST skip these IDs so it doesn't promote a stale
        // pending+payload-NULL row to 'error' while its proc entry is
        // still recoverable on the next cycle.
        std::vector<std::string> deferred_version_ids;
    };

    // Sweep the Redis processing-list keyspace for workers whose
    // heartbeat has expired; PG-touch + atomic-LMOVE entries back to
    // source queues. Returns the transferred count plus a deferred
    // set for sweep #1 to skip.
    drogon::Task<ReapResult> reap_orphan_processing_lists();

    // Sleep for opts_.interval, waking every opts_.sleep_chunk to check
    // shutdown_ so a SIGTERM does not block for up to a full interval.
    drogon::Task<void> interruptible_sleep();

    drogon::orm::DbClientPtr db_;
    ShutdownPredicate        shutdown_;
    Options                  opts_;
    std::atomic<std::size_t> total_swept_{0};
};

} // namespace wikore::scheduler