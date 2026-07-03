#include "wikore/scheduler/polling_fallback.hpp"
#include "wikore/adapters/postgres/unit_of_work.hpp"
#include "wikore/redis.hpp"
#include <drogon/drogon.h>
#include <glaze/glaze.hpp>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <chrono>
#include <cassert>
#include <format>
#include <optional>
#include <stdexcept>

namespace wikore::scheduler {

// Minimal JSON shape extracted from a processing-list payload for
// sweep #3's DB-consulted routing. Defined at namespace scope (not
// anon) so the glz::meta specialisation at the bottom of this TU can
// name it. Use error_on_unknown_keys=false when reading so producer
// payloads with extra fields (and whitespace-formatted JSON variants)
// parse cleanly -- field order/formatting is not part of our queue
// contract.
struct PayloadShape {
    std::string company_id;
    std::string document_version_id;
};

namespace {

constexpr std::string_view kQueuePrefix     = "lr:ingest:q:";
constexpr std::string_view kProcPrefix      = "lr:ingest:proc:";
constexpr std::string_view kHeartbeatPrefix = "lr:ingest:hb:";

drogon::Task<void> co_sleep(std::chrono::milliseconds    d,
                            const std::function<void()>& on_armed = {})
{
    auto* loop = drogon::app().getLoop();
    struct Awaiter {
        trantor::EventLoop*           loop;
        std::chrono::milliseconds     delay;
        const std::function<void()>*  on_armed;
        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<> h) {
            loop->runAfter(static_cast<double>(delay.count()) / 1000.0,
                           [h]() mutable { h.resume(); });
            if (*on_armed) {
                try {
                    (*on_armed)();
                } catch (const std::exception& ex) {
                    spdlog::error("[polling-fallback] sleep observer failed: {}",
                                  ex.what());
                } catch (...) {
                    spdlog::error("[polling-fallback] sleep observer failed with "
                                  "a non-standard exception");
                }
            }
        }
        void await_resume() const noexcept {}
    };
    co_await Awaiter{loop, d, &on_armed};
}

} // namespace

PollingFallback::PollingFallback(drogon::orm::DbClientPtr db,
                                   ShutdownPredicate        shutdown_requested,
                                   Options                  opts)
    : db_(std::move(db))
    , shutdown_(std::move(shutdown_requested))
    , opts_(std::move(opts))
{
    if (opts_.sleep_chunk <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("PollingFallback sleep_chunk must be positive");
    }
    if (opts_.interval <= std::chrono::seconds::zero()) {
        throw std::invalid_argument("PollingFallback interval must be positive");
    }
}

drogon::Task<int> PollingFallback::sweep_once()
{
    // Phase ordering:
    //   1. Orphan proc-list recovery (sweep #3) FIRST. This is the
    //      LMOVE-window recovery: a worker that died between LMOVE and
    //      payload-persist leaves the row at 'pending' with payload IS
    //      NULL while the proc list holds the sole copy. Running this
    //      phase before sweep #1 means the transfer-back (which also
    //      writes ingest_job_payload to DB) happens BEFORE sweep #1
    //      would otherwise see "pending + no payload" and promote to
    //      'error'. Without this ordering, the sole-copy job would be
    //      destroyed in the same sweep_once() it was recoverable.
    //
    //      Reap also returns a `deferred_version_ids` set: version IDs
    //      whose pre-transfer touch UPDATE failed. Sweep #1 below MUST
    //      skip these IDs, otherwise it can promote a still-recoverable
    //      pending+payload-NULL row to 'error' while its proc entry is
    //      still on Redis waiting for the next reaper cycle.
    //
    //   2. DB-state sweeps (#1 + #2) inside one xact_advisory_lock so
    //      concurrent schedulers don't double-sweep.
    //
    // Phase 1 runs OUTSIDE the xact_lock because it does long-running
    // Redis I/O; holding a Postgres transaction lock through that would
    // serialise the wrong work. Concurrency is handled per-payload via
    // Redis::transfer_proc_to_source (atomic LREM-then-conditional-LPUSH
    // in a single EVAL), so two schedulers cannot both LPUSH the same
    // payload to a source queue.
    const auto reap_result = co_await reap_orphan_processing_lists();
    const int reaped = reap_result.transferred;
    const auto& deferred_ids = reap_result.deferred_version_ids;

    // UnitOfWork::begin can throw if the DB pool is exhausted or the
    // server is unreachable; wrap to keep the polling loop alive.
    std::optional<postgres::UnitOfWork> uow_opt;
    try {
        uow_opt.emplace(co_await postgres::UnitOfWork::begin(db_));
    } catch (const drogon::orm::DrogonDbException& ex) {
        spdlog::error("[polling-fallback] UnitOfWork::begin failed: {}; "
                      "skipping DB sweeps this cycle (reaped {} so far)",
                      ex.base().what(), reaped);
        co_return reaped;
    } catch (const std::exception& ex) {
        spdlog::error("[polling-fallback] UnitOfWork::begin threw: {}; "
                      "skipping DB sweeps this cycle (reaped {})",
                      ex.what(), reaped);
        co_return reaped;
    }
    auto& uow = *uow_opt;

    bool got_lock = false;
    try {
        auto locked = co_await uow.exec(
            "SELECT pg_try_advisory_xact_lock(hashtext($1)) AS got",
            opts_.advisory_lock_name);
        got_lock = !locked.empty() && locked[0]["got"].as<bool>();
    } catch (const drogon::orm::DrogonDbException& ex) {
        spdlog::warn("[polling-fallback] xact_lock failed: {}", ex.base().what());
        uow.rollback();
        co_return reaped;
    }
    if (!got_lock) {
        if (auto r = co_await uow.commit(); !r)
            spdlog::warn("[polling-fallback] no-lock commit failed: {}", r.error().message);
        co_return reaped;
    }

    int swept_pending = 0;
    int swept_processing = 0;
    // Rows to requeue after commit (company_id, version_id, payload,
    // version_id again for rollback if LPUSH ultimately fails).
    std::vector<std::tuple<std::string, std::string, std::string,
                            std::string>> to_requeue;

    try {
        // Sweep 1: stale 'pending' rows.
        //
        // Predicate is updated_at (not created_at): when sweep #2 resets a
        // 'processing' row back to 'pending', the BEFORE UPDATE
        // set_updated_at trigger bumps updated_at to now(). Using
        // created_at here would have promoted those recovered rows to
        // 'error' on the next sweep, immediately undoing the recovery.
        //
        // Behaviour split by whether ingest_job_payload was persisted:
        //
        //   * payload IS NOT NULL: this row was claimed by a worker that
        //     ALSO died after persisting the payload but before flipping
        //     to 'processing' (a narrow window inside dispatch). The job
        //     is recoverable; treat exactly like the stuck-processing
        //     case and requeue if retry budget allows.
        //
        //   * payload IS NULL: api committed but no payload was ever
        //     written -- either the api crashed before LPUSH or the
        //     worker died before persisting. There's nothing to requeue;
        //     promote to 'error' -- UNLESS this version is in the
        //     `deferred_ids` set (its proc entry exists but the reaper
        //     couldn't touch updated_at this cycle).
        auto pending_stuck = co_await uow.exec(std::format(R"(
            SELECT id::text                          AS id,
                   company_id::text                  AS company_id,
                   ingest_retry_count                AS retries,
                   COALESCE(ingest_job_payload::text, '') AS payload
            FROM   document_versions
            WHERE  ingest_status = 'pending'
              AND  updated_at    < now() - interval '{} minutes'
            FOR    UPDATE
        )", opts_.stuck_threshold.count()));

        for (const auto& r : pending_stuck) {
            const auto id      = r["id"].as<std::string>();
            const auto co_id   = r["company_id"].as<std::string>();
            const auto retries = r["retries"].as<int>();
            const auto payload = r["payload"].as<std::string>();

            // Deferred from reap_orphan_processing_lists: skip this
            // cycle so a touch-UPDATE failure doesn't get terminally
            // promoted while its proc entry waits for the next reaper.
            if (std::find(deferred_ids.begin(), deferred_ids.end(), id)
                != deferred_ids.end())
            {
                spdlog::info("[polling-fallback] skipping stuck-pending "
                             "version {} this cycle (reaper deferred)",
                             id);
                continue;
            }

            const bool can_requeue =
                !payload.empty() && retries < opts_.max_resume_attempts;

            if (can_requeue) {
                // Increment retry inside the transaction so it's atomic
                // with the bookkeeping; LPUSH happens after commit. The
                // row is already 'pending' so no status flip needed.
                // Clear ingest_claim_token: any prior worker's token
                // must not survive a recovery cycle, else the next
                // worker's CAS-protected mark_done would falsely fail.
                co_await uow.exec(R"(
                    UPDATE document_versions
                    SET    ingest_retry_count = ingest_retry_count + 1,
                           error_msg          = NULL,
                           ingest_claim_token = NULL
                    WHERE  id = $1::uuid
                )", id);
                to_requeue.emplace_back(co_id, id, payload, id);
                ++swept_pending;
                spdlog::info("[polling-fallback] recovering stuck-pending "
                             "version {} (payload persisted, retries={})",
                             id, retries);
            } else {
                co_await uow.exec(std::format(R"(
                    UPDATE document_versions
                    SET    ingest_status      = 'error',
                           error_msg          = '{}',
                           ingest_job_payload = NULL,
                           ingest_claim_token = NULL
                    WHERE  id = $1::uuid
                )", payload.empty()
                    ? "stuck pending; no ingest job materialized "
                      "(api crash before LPUSH, or worker crashed before "
                      "persisting payload). Re-trigger the upload."
                    : "stuck pending; resume budget exhausted. "
                      "Inspect for poison input and re-trigger."), id);
                spdlog::warn("[polling-fallback] promoted stuck-pending "
                             "version {} to 'error' (retries={}, payload={})",
                             id, retries, payload.empty() ? "missing" : "present");
                ++swept_pending;
            }
        }

        // Sweep 2: rows stuck at 'processing' beyond the threshold.
        //
        // Same recovery semantics as sweep #1's pending+payload branch:
        // requeue if payload + budget, else error. The two sweeps share
        // the to_requeue list so a single post-commit LPUSH loop handles
        // both.
        //
        // stuck_threshold must exceed ingest_worker.deadline_per_job +
        // safety margin so that legitimately-long ingests aren't reaped.
        auto stuck = co_await uow.exec(std::format(R"(
            SELECT id::text                          AS id,
                   company_id::text                  AS company_id,
                   ingest_retry_count                AS retries,
                   COALESCE(ingest_job_payload::text, '') AS payload
            FROM   document_versions
            WHERE  ingest_status = 'processing'
              AND  updated_at    < now() - interval '{} minutes'
            FOR    UPDATE
        )", opts_.stuck_threshold.count()));

        for (const auto& r : stuck) {
            const auto id      = r["id"].as<std::string>();
            const auto co_id   = r["company_id"].as<std::string>();
            const auto retries = r["retries"].as<int>();
            const auto payload = r["payload"].as<std::string>();

            const bool can_requeue =
                !payload.empty() && retries < opts_.max_resume_attempts;

            if (can_requeue) {
                // Reset to 'pending' AND clear ingest_claim_token: the
                // crashed worker's token must not survive the recovery
                // cycle, else the NEXT worker's CAS-protected mark_done
                // would falsely fail because the stored token doesn't
                // match the next worker's (fresh) token.
                co_await uow.exec(R"(
                    UPDATE document_versions
                    SET    ingest_status       = 'pending',
                           error_msg           = NULL,
                           ingest_retry_count  = ingest_retry_count + 1,
                           ingest_claim_token  = NULL
                    WHERE  id = $1::uuid
                )", id);
                to_requeue.emplace_back(co_id, id, payload, id);
                ++swept_processing;
            } else {
                co_await uow.exec(std::format(R"(
                    UPDATE document_versions
                    SET    ingest_status      = 'error',
                           error_msg          = '{}',
                           ingest_job_payload = NULL,
                           ingest_claim_token = NULL
                    WHERE  id = $1::uuid
                )", payload.empty()
                    ? "stuck processing; no recoverable job payload "
                      "(worker crashed before persisting payload). "
                      "Re-trigger the upload."
                    : "stuck processing; resume budget exhausted "
                      "(worker repeatedly crashed on this job). "
                      "Inspect for poison input and re-trigger."), id);
                spdlog::warn("[polling-fallback] promoted stuck-processing "
                             "version {} to 'error' (retries={}, payload={})",
                             id, retries, payload.empty() ? "missing" : "present");
                ++swept_processing;
            }
        }

    } catch (const drogon::orm::DrogonDbException& ex) {
        spdlog::error("[polling-fallback] sweep failed: {}", ex.base().what());
        uow.rollback();
        co_return 0;
    }

    if (auto r = co_await uow.commit(); !r) {
        spdlog::error("[polling-fallback] sweep commit failed: {}", r.error().message);
        co_return 0;
    }

    // Post-commit LPUSH with in-loop retry. A transient Redis blip during
    // requeue (after the DB commit) used to leave the row in 'pending'
    // with no queue entry, and the next sweep #1 would promote it to
    // 'error' -- losing the kill/restart contract for one extra failure.
    //
    // Now: retry the LPUSH up to N times with short waits. If still
    // failing, roll back the DB transition (decrement retry, set status
    // back to 'processing' so sweep #2 catches it next cycle, OR leave
    // pending+payload so sweep #1 catches it next cycle if it was a
    // pending-recovery). Either way, the job stays recoverable.
    constexpr int  kPushAttempts   = 3;
    constexpr auto kPushRetryDelay = std::chrono::milliseconds(200);
    for (const auto& [co_id, version_id, payload, _] : to_requeue) {
        const auto queue_key = std::string(kQueuePrefix) + co_id;
        long long pushed = -1;
        for (int attempt = 0; attempt < kPushAttempts; ++attempt) {
            pushed = wikore::Redis::lpush(queue_key, payload);
            if (pushed >= 0) break;
            spdlog::warn("[polling-fallback] LPUSH attempt {} to {} failed "
                         "for version {}; retrying",
                         attempt + 1, queue_key, version_id);
            co_await co_sleep(kPushRetryDelay);
        }
        if (pushed >= 0) {
            spdlog::info("[polling-fallback] requeued version {} to {} "
                         "(queue depth={})", version_id, queue_key, pushed);
            continue;
        }

        // All LPUSH attempts failed. Roll the DB row back so the job is
        // still discoverable on the next sweep. Best-effort: if this
        // rollback also fails, the row is in 'pending' with payload set,
        // which sweep #1 will pick up next cycle as a recovery candidate.
        spdlog::error("[polling-fallback] LPUSH to {} failed {} times for "
                      "version {}; rolling back DB transition so next sweep "
                      "retries", queue_key, kPushAttempts, version_id);
        try {
            co_await db_->execSqlCoro(
                "UPDATE document_versions "
                "SET    ingest_retry_count = GREATEST(ingest_retry_count - 1, 0) "
                "WHERE  id = $1::uuid",
                version_id);
        } catch (const drogon::orm::DrogonDbException& ex) {
            spdlog::error("[polling-fallback] rollback retry-count for {} "
                          "failed: {}; row stays pending+payload and will "
                          "be picked up next sweep",
                          version_id, ex.base().what());
        }
    }

    // Sweep #3 already ran at the top of sweep_once() so its
    // recoverable transfers landed before sweeps #1/#2 read DB state.
    if (reaped > 0)
        total_swept_.fetch_add(static_cast<std::size_t>(reaped));

    const int swept = swept_pending + swept_processing + reaped;
    if (swept_pending + swept_processing > 0)
        total_swept_.fetch_add(
            static_cast<std::size_t>(swept_pending + swept_processing));
    co_return swept;
}

drogon::Task<PollingFallback::ReapResult>
PollingFallback::reap_orphan_processing_lists()
{
    // Cleanup-and-recover sweep for orphan processing lists. For each
    // proc-key whose heartbeat is confirmed missing:
    //
    //   1. Split the proc-key to extract the TRUSTED source tenant
    //      (encoded as the suffix after the LAST ':'). Worker_id and
    //      tenant_uuid both lack ':' so the split is unambiguous. The
    //      worker writes proc-keys as:
    //         lr:ingest:proc:{worker_id}:{tenant_uuid}
    //      Using the key's tenant (not the untrusted payload's
    //      company_id) prevents a malformed payload from rerouting a
    //      job to a different tenant's queue.
    //
    //   2. Parse the payload with glaze ONLY to extract a logging-
    //      friendly version_id; the routing is already decided by (1).
    //      Malformed payloads are LREMed defensively.
    //
    //   3. Touch document_versions.updated_at FIRST via a CAS-style
    //      no-op UPDATE (guarded by ingest_status='pending') so
    //      sweep #1 running later in this same sweep_once() doesn't
    //      see a stale pending+payload-NULL row and promote it to
    //      'error' while the LMOVE-window recovery is still in flight.
    //      The BEFORE UPDATE set_updated_at trigger bumps updated_at
    //      to now() regardless of which column we touch.
    //
    //   4. ONLY IF (3) succeeded: atomic Lua transfer (LREM-then-
    //      conditional-LPUSH) so two concurrent reapers cannot both
    //      push the same entry.
    //
    // Ordering matters: if we did the Redis transfer first and the
    // UPDATE then failed, the proc entry would be gone (lost from
    // Redis) AND updated_at would still be stale, so sweep #1 would
    // promote the recovered row to 'error', losing the job. By
    // updating PG first, a DB failure leaves the proc entry intact
    // for the next reaper cycle; an updated_at that's slightly ahead
    // of the actual transfer is harmless (sweep #1 just skips the
    // row that cycle).
    //
    // Duplicate-delivery safety is provided at the worker side by
    // claim_for_processing's CAS (only flips pending->processing).
    // Resurrection of terminal rows and double-processing in-flight
    // rows are both absorbed there, so unconditional transfer-back is
    // safe.
    //
    // tri-state EXISTS: only EXISTS=0 (confirmed missing) triggers
    // cleanup; EXISTS=-1 (Redis error) is skip-and-retry.
    auto proc_keys = wikore::Redis::scan_keys(
        std::string(kProcPrefix) + "*", /*limit=*/256);
    ReapResult result;
    if (proc_keys.empty()) co_return result;

    // UUID-shape gate so a malformed proc-key (left by a manual
    // operator, an old test, or a different deploy) doesn't get
    // dispatched. Same 8-4-4-4-12 hex format check used by the worker.
    auto is_uuid_v4_ish = [](std::string_view s) noexcept {
        if (s.size() != 36) return false;
        for (std::size_t i = 0; i < 36; ++i) {
            if (i == 8 || i == 13 || i == 18 || i == 23) {
                if (s[i] != '-') return false;
            } else {
                const char c = s[i];
                const bool hex = (c >= '0' && c <= '9')
                              || (c >= 'a' && c <= 'f')
                              || (c >= 'A' && c <= 'F');
                if (!hex) return false;
            }
        }
        return true;
    };

    for (const auto& proc_key : proc_keys) {
        if (proc_key.size() <= kProcPrefix.size()) continue;
        const auto worker_and_tenant = proc_key.substr(kProcPrefix.size());

        // Split worker_id and tenant on the LAST ':'.
        // Pre-PR #19/7 keys without ':' suffix (worker-only proc-keys)
        // have no trusted tenant; skip them rather than guess.
        const auto sep = worker_and_tenant.rfind(':');
        if (sep == std::string::npos) {
            spdlog::warn("[polling-fallback] reap: proc-key {} has no "
                         "tenant suffix; skipping (legacy key from older "
                         "worker?)", proc_key);
            continue;
        }
        const auto worker_id  = worker_and_tenant.substr(0, sep);
        const auto src_tenant = worker_and_tenant.substr(sep + 1);

        // Defence-in-depth: the proc-key suffix MUST be a valid UUID.
        // If not, the key is malformed (operator typo, foreign deploy
        // sharing Redis, etc.) and we should not route entries to it.
        if (!is_uuid_v4_ish(src_tenant)) {
            spdlog::warn("[polling-fallback] reap: proc-key {} has "
                         "non-UUID tenant suffix '{}'; skipping",
                         proc_key, src_tenant);
            continue;
        }

        const auto hb_key     = std::string(kHeartbeatPrefix) + worker_id;

        const int hb = wikore::Redis::exists(hb_key);
        if (hb != 0) continue;   // alive (1) OR Redis error (-1)

        spdlog::warn("[polling-fallback] worker {} heartbeat missing; "
                     "transferring its processing list (tenant={})",
                     worker_id, src_tenant);

        const auto src_key = std::string(kQueuePrefix) + src_tenant;

        auto entries = wikore::Redis::lrange(proc_key, 0, -1);
        for (const auto& payload : entries) {
            // Glaze parse just for logging the version_id. The
            // routing tenant is the trusted one from the proc-key.
            // We defensively LREM payloads that are syntactically
            // malformed (so the dead worker's list doesn't grow
            // unbounded).
            PayloadShape shape;
            if (auto err = glz::read<
                    glz::opts{.error_on_unknown_keys = false}>(shape, payload);
                err)
            {
                spdlog::warn("[polling-fallback] reap: malformed payload on "
                             "{}: {}; LREM", proc_key,
                             glz::format_error(err, payload));
                wikore::Redis::lrem(proc_key, 1, payload);
                continue;
            }

            // Defensive copy of version_id: glaze writes 8-byte SIMD
            // padding past size() then resize-down without rewriting
            // the trailing nul; libpq's text-format strlen over-reads.
            // (ptr, size) round-trips through libstdc++'s _M_construct
            // which guarantees nul-termination.
            std::string version_id;
            version_id.reserve(shape.document_version_id.size() + 1);
            version_id.assign(shape.document_version_id.data(),
                              shape.document_version_id.size());

            // (3) Touch updated_at FIRST so sweep #1 (running later in
            // this same sweep_once under the xact_lock) doesn't see a
            // stale pending+payload-NULL row and promote to 'error'
            // before a worker has had a chance to CAS-claim the
            // requeued entry. Guard with ingest_status='pending' so we
            // never touch a terminal or in-flight row.
            //
            // The query is no-op safe: SET ingest_retry_count =
            // ingest_retry_count doesn't change the column value, but
            // the BEFORE UPDATE set_updated_at trigger fires and
            // refreshes updated_at to now().
            //
            // Ordering matters: if the DB query fails we leave the
            // proc entry untouched AND record the version_id in
            // deferred_version_ids. sweep_once() reads this and
            // excludes the deferred IDs from sweep #1's loop, so a
            // touch failure here cannot result in a terminal promotion
            // by the very same sweep_once().
            try {
                std::string vid_copy = version_id;
                co_await db_->execSqlCoro(
                    std::string{"UPDATE document_versions "
                                "SET    ingest_retry_count = ingest_retry_count "
                                "WHERE  company_id    = $1::uuid "
                                "  AND  id            = $2::uuid "
                                "  AND  ingest_status = 'pending'"},
                    std::string(src_tenant),
                    std::move(vid_copy));
            } catch (const drogon::orm::DrogonDbException& ex) {
                spdlog::warn("[polling-fallback] reap: touch updated_at "
                             "before transfer for {} failed: {}; leaving "
                             "proc entry for next-cycle retry, deferring "
                             "from sweep #1 this cycle",
                             shape.document_version_id, ex.base().what());
                result.deferred_version_ids.push_back(std::move(version_id));
                continue;
            }

            // (4) Atomic Redis transfer. If this fails the proc entry
            // is left in place AND the timestamp bump is now slightly
            // ahead of the actual transfer -- harmless: the next
            // reaper cycle will retry the transfer, and sweep #1 will
            // skip this row because updated_at is fresh. The slight
            // delay (one sweep cycle) is acceptable.
            const int outcome = wikore::Redis::transfer_proc_to_source(
                proc_key, src_key, payload);
            if (outcome < 0) {
                spdlog::error("[polling-fallback] reap: atomic transfer "
                              "for version {} failed; entry remains on {} "
                              "for next-cycle retry",
                              shape.document_version_id, proc_key);
                continue;
            }
            if (outcome == 0) {
                spdlog::info("[polling-fallback] reap: entry for version "
                             "{} already transferred by a concurrent reaper",
                             shape.document_version_id);
                continue;
            }

            ++result.transferred;
            spdlog::info("[polling-fallback] reap: transferred entry for "
                         "version {} from worker {} back to {} (trusted "
                         "tenant)",
                         shape.document_version_id, worker_id, src_key);
        }
    }
    co_return result;
}

drogon::Task<void> PollingFallback::run()
{
    spdlog::info("[polling-fallback] starting; interval={}s threshold={}min",
                 opts_.interval.count(), opts_.stuck_threshold.count());
    while (!shutdown_()) {
        // Defensive outer catch: any unhandled exception escaping
        // sweep_once() (UnitOfWork::begin, glaze, libpq, ...) must NOT
        // terminate the long-running recovery coroutine. A transient
        // PG outage at sweep boundary is exactly the case where we
        // need recovery to keep running.
        try {
            int n = co_await sweep_once();
            if (n > 0)
                spdlog::info("[polling-fallback] swept {} stuck versions", n);
        } catch (const drogon::orm::DrogonDbException& ex) {
            spdlog::error("[polling-fallback] sweep_once raised DB "
                          "exception: {}; backing off and continuing",
                          ex.base().what());
        } catch (const std::exception& ex) {
            spdlog::error("[polling-fallback] sweep_once raised: {}; "
                          "backing off and continuing", ex.what());
        } catch (...) {
            spdlog::error("[polling-fallback] sweep_once raised unknown "
                          "exception; backing off and continuing");
        }
        co_await interruptible_sleep();
    }
    spdlog::info("[polling-fallback] exiting; total_swept={}", total_swept_.load());
}

drogon::Task<void> PollingFallback::interruptible_sleep()
{
    const auto deadline =
        std::chrono::steady_clock::now() +
        std::chrono::duration_cast<std::chrono::nanoseconds>(opts_.interval);
    while (!shutdown_()) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) break;
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        co_await co_sleep(std::min(remaining, opts_.sleep_chunk), opts_.on_sleep_armed);
    }
}

} // namespace wikore::scheduler

template <>
struct glz::meta<wikore::scheduler::PayloadShape> {
    using T = wikore::scheduler::PayloadShape;
    static constexpr auto value = glz::object(
        "company_id",          &T::company_id,
        "document_version_id", &T::document_version_id);
};