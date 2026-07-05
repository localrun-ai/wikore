#include "wikore/scheduler/edge_vector_cleanup_worker.hpp"
#include "wikore/adapters/postgres/error_mapper.hpp"
#include <drogon/drogon.h>
#include <spdlog/spdlog.h>
#include <chrono>
#include <format>
#include <string>
#include <unistd.h>

namespace wikore::scheduler {

namespace {

// Cancellable sleep on the Drogon loop; matches the pattern used by the
// other scheduler workers (kept private to this TU).
drogon::Task<void> co_sleep(std::chrono::milliseconds d)
{
    auto* loop = drogon::app().getLoop();
    struct Awaiter {
        trantor::EventLoop*       loop;
        std::chrono::milliseconds delay;
        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<> h) {
            loop->runAfter(static_cast<double>(delay.count()) / 1000.0,
                           [h]() mutable { h.resume(); });
        }
        void await_resume() const noexcept {}
    };
    co_await Awaiter{loop, d};
}

} // namespace

EdgeVectorCleanupWorker::EdgeVectorCleanupWorker(
    drogon::orm::DbClientPtr              db,
    std::shared_ptr<rag::VectorStorePort> vector_store,
    ShutdownPredicate                     shutdown_requested,
    Options                               opts)
    : db_(std::move(db))
    , vector_store_(std::move(vector_store))
    , shutdown_(std::move(shutdown_requested))
    , opts_(std::move(opts))
{
    char host[256] = {0};
    gethostname(host, sizeof(host) - 1);
    worker_id_ = std::format("{}:{}", host, getpid());
}

// ---------------------------------------------------------------------------
// Claim a batch of pending qdrant_delete_edge_points events. Same
// FOR UPDATE SKIP LOCKED pattern as the other outbox consumers. Payload
// UUID array is unmarshalled into a std::vector<std::string> here so
// process() does not need to touch JSON. Stale claims are reaped first.
// ---------------------------------------------------------------------------

drogon::Task<std::vector<EdgeVectorCleanupWorker::ClaimedEvent>>
EdgeVectorCleanupWorker::claim_batch()
{
    co_await reap_stale_claims();

    constexpr auto kSql = R"(
        UPDATE outbox_events e
        SET    claimed_at    = now(),
               claimed_by    = $3,
               attempt_count = e.attempt_count + 1
        WHERE  e.id IN (
            SELECT id
            FROM   outbox_events
            WHERE  job_type        = 'qdrant_delete_edge_points'
              AND  completed_at    IS NULL
              AND  claimed_at      IS NULL
              AND  attempt_count   < $2::int
              AND  next_attempt_at <= now()
            ORDER BY next_attempt_at
            FOR UPDATE SKIP LOCKED
            LIMIT $1::int
        )
        RETURNING
               e.id::text                                       AS id,
               e.company_id::text                               AS company_id,
               e.aggregate_id::text                             AS edge_id,
               -- Unmarshal the qdrant_point_ids JSONB array into a
               -- text[] so the C++ side receives std::vector<std::string>
               -- directly, avoiding a per-worker JSON dependency.
               ARRAY(
                   SELECT jsonb_array_elements_text(
                       COALESCE(e.payload->'qdrant_point_ids', '[]'::jsonb))
               ) AS point_ids
    )";

    std::vector<ClaimedEvent> events;
    try {
        auto rows = co_await db_->execSqlCoro(kSql,
            opts_.batch_size, opts_.max_attempts, worker_id_);
        events.reserve(rows.size());
        for (const auto& r : rows) {
            ClaimedEvent ev{
                .id         = r["id"].as<std::string>(),
                .company_id = r["company_id"].as<std::string>(),
                .edge_id    = r["edge_id"].as<std::string>(),
                .point_ids  = {},
            };
            // Drogon's asArray returns vector<shared_ptr<T>>; flatten it.
            for (const auto& sp : r["point_ids"].asArray<std::string>())
                if (sp) ev.point_ids.push_back(*sp);
            events.push_back(std::move(ev));
        }
    } catch (const drogon::orm::DrogonDbException& ex) {
        spdlog::warn("[edge-cleanup-worker] claim_batch failed: {}", ex.base().what());
    }
    co_return events;
}

drogon::Task<int> EdgeVectorCleanupWorker::reap_stale_claims()
{
    try {
        auto rows = co_await db_->execSqlCoro(std::format(R"(
            UPDATE outbox_events
            SET    claimed_at    = NULL,
                   claimed_by    = NULL,
                   attempt_count = GREATEST(attempt_count - 1, 0),
                   last_error    = COALESCE(last_error, '') ||
                                   ' [reaped: stale claim by ' ||
                                   COALESCE(claimed_by, '?') || ']'
            WHERE  job_type     = 'qdrant_delete_edge_points'
              AND  completed_at IS NULL
              AND  claimed_at IS NOT NULL
              AND  claimed_at < now() - interval '{} minutes'
            RETURNING id::text
        )", opts_.claim_lease.count()));
        const int n = static_cast<int>(rows.size());
        for (const auto& r : rows)
            spdlog::warn("[edge-cleanup-worker] reaped stale claim on event {} (age > {} min)",
                         r["id"].as<std::string>(), opts_.claim_lease.count());
        co_return n;
    } catch (const drogon::orm::DrogonDbException& ex) {
        spdlog::warn("[edge-cleanup-worker] reap_stale_claims failed: {}", ex.base().what());
        co_return 0;
    }
}

drogon::Task<int> EdgeVectorCleanupWorker::release_my_claims()
{
    try {
        auto rows = co_await db_->execSqlCoro(R"(
            UPDATE outbox_events
            SET    claimed_at    = NULL,
                   claimed_by    = NULL,
                   attempt_count = GREATEST(attempt_count - 1, 0)
            WHERE  job_type     = 'qdrant_delete_edge_points'
              AND  completed_at IS NULL
              AND  claimed_by   = $1
            RETURNING id::text
        )", worker_id_);
        const int n = static_cast<int>(rows.size());
        if (n > 0)
            spdlog::info("[edge-cleanup-worker] {} released {} unprocessed claim(s) on shutdown",
                         worker_id_, n);
        co_return n;
    } catch (const drogon::orm::DrogonDbException& ex) {
        spdlog::warn("[edge-cleanup-worker] release_my_claims failed: {}", ex.base().what());
        co_return 0;
    }
}

// ---------------------------------------------------------------------------
// Process one claimed event. VectorStorePort::delete_points_by_id is
// idempotent (Qdrant's POST /points/delete with a points list treats missing
// ids as no-ops), so a redelivered event is safe; the outbox
// `(company_id, job_type, idempotency_key)` UNIQUE prevents the trigger
// from re-enqueuing the same delete twice within a retry window.
// ---------------------------------------------------------------------------

drogon::Task<Result<void>>
EdgeVectorCleanupWorker::process(const ClaimedEvent& ev)
{
    // Empty payload means the trigger fired for an edge that had no
    // embeddings — the trigger already skips the enqueue in that case,
    // but be defensive.
    if (ev.point_ids.empty())
        co_return Result<void>{};

    if (auto r = co_await vector_store_->delete_points_by_id(ev.company_id, ev.point_ids); !r)
        co_return std::unexpected(r.error());
    co_return Result<void>{};
}

drogon::Task<void>
EdgeVectorCleanupWorker::mark_completed(const std::string& event_id)
{
    try {
        co_await db_->execSqlCoro(
            "UPDATE outbox_events SET completed_at = now(), claimed_at = NULL "
            "WHERE id = $1::uuid",
            event_id);
    } catch (const drogon::orm::DrogonDbException& ex) {
        spdlog::error("[edge-cleanup-worker] mark_completed({}) failed: {}",
                      event_id, ex.base().what());
    }
}

drogon::Task<void>
EdgeVectorCleanupWorker::mark_failed(const std::string& event_id,
                                     std::string_view  reason)
{
    try {
        // Exponential backoff capped at 300s, matching OutboxWorker /
        // ResyncWorker so a Qdrant blip does not burn the retry budget.
        co_await db_->execSqlCoro(R"(
            UPDATE outbox_events
            SET    claimed_at      = NULL,
                   claimed_by      = NULL,
                   last_error      = $2,
                   next_attempt_at = now()
                                   + (LEAST(300, POWER(2, attempt_count)::int)
                                      * interval '1 second')
            WHERE  id = $1::uuid
        )", event_id, std::string(reason));
    } catch (const drogon::orm::DrogonDbException& ex) {
        spdlog::error("[edge-cleanup-worker] mark_failed({}) failed: {}",
                      event_id, ex.base().what());
    }
}

drogon::Task<int> EdgeVectorCleanupWorker::drain_once()
{
    auto batch = co_await claim_batch();
    int processed = 0;
    for (const auto& ev : batch) {
        if (shutdown_()) break;
        auto r = co_await process(ev);
        if (r) {
            co_await mark_completed(ev.id);
            events_completed_.fetch_add(1);
            ++processed;
        } else {
            const auto& err = r.error();
            co_await mark_failed(ev.id, err.message);
            events_failed_.fetch_add(1);
            spdlog::warn("[edge-cleanup-worker] event={} edge={} failed: {}",
                         ev.id, ev.edge_id, err.message);
        }
    }
    co_return processed;
}

drogon::Task<void> EdgeVectorCleanupWorker::run()
{
    spdlog::info("[edge-cleanup-worker] {} starting; poll={}ms batch_size={} lease={}min",
                 worker_id_, opts_.poll_interval.count(), opts_.batch_size,
                 opts_.claim_lease.count());
    while (!shutdown_()) {
        int processed = co_await drain_once();
        if (processed < opts_.batch_size)
            co_await co_sleep(opts_.poll_interval);
    }
    co_await release_my_claims();
    spdlog::info("[edge-cleanup-worker] {} drained; exiting", worker_id_);
}

} // namespace wikore::scheduler
