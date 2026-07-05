#include "wikore/scheduler/resync_worker.hpp"
#include "wikore/adapters/postgres/error_mapper.hpp"
#include "wikore/adapters/postgres/unit_of_work.hpp"
#include "wikore/rag/types.hpp"
#include <drogon/drogon.h>
#include <spdlog/spdlog.h>
#include <chrono>
#include <format>
#include <map>
#include <optional>
#include <string>
#include <unistd.h>

namespace wikore::scheduler {

namespace {

// Cancellable sleep on the Drogon loop (same shape as the ingest/outbox
// workers; kept private to this TU to avoid a public dependency surface).
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

// Render a set of UUID text strings as a Postgres array literal: {a,b,c}.
// The elements come from PG (fetch_access_scopes) and are already valid UUIDs,
// so no escaping is required.
std::string to_pg_uuid_array(const std::vector<std::string>& ids)
{
    std::string s = "{";
    for (size_t i = 0; i < ids.size(); ++i) {
        if (i) s += ',';
        s += ids[i];
    }
    s += '}';
    return s;
}

} // namespace

ResyncWorker::ResyncWorker(drogon::orm::DbClientPtr                  db,
                           VectorStoreForCollection                  store_for_collection,
                           std::shared_ptr<ingest::DocumentRepoPort> repo,
                           ShutdownPredicate                         shutdown_requested,
                           Options                                   opts)
    : db_(std::move(db))
    , store_for_collection_(std::move(store_for_collection))
    , repo_(std::move(repo))
    , shutdown_(std::move(shutdown_requested))
    , opts_(std::move(opts))
{
    char host[256] = {0};
    gethostname(host, sizeof(host) - 1);
    worker_id_ = std::format("{}:{}", host, getpid());
}

// ---------------------------------------------------------------------------
// Claim a batch of pending qdrant_resync_chunk_acl events. FOR UPDATE SKIP
// LOCKED lets multiple instances claim disjoint events. Stale claims are
// reaped first so a crashed worker never strands events.
// ---------------------------------------------------------------------------

drogon::Task<std::vector<ResyncWorker::ClaimedEvent>> ResyncWorker::claim_batch()
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
            WHERE  job_type        = 'qdrant_resync_chunk_acl'
              AND  completed_at    IS NULL
              AND  claimed_at      IS NULL
              AND  attempt_count   < $2::int
              AND  next_attempt_at <= now()
            ORDER BY next_attempt_at
            FOR UPDATE SKIP LOCKED
            LIMIT $1::int
        )
        RETURNING
               e.id::text                                        AS id,
               e.company_id::text                                AS company_id,
               e.aggregate_id::text                              AS document_id,
               COALESCE((e.payload->>'acl_version')::bigint, 0)  AS acl_version
    )";

    std::vector<ClaimedEvent> events;
    try {
        auto rows = co_await db_->execSqlCoro(kSql,
            opts_.batch_size, opts_.max_attempts, worker_id_);
        events.reserve(rows.size());
        for (const auto& r : rows) {
            events.push_back(ClaimedEvent{
                .id          = r["id"].as<std::string>(),
                .company_id  = r["company_id"].as<std::string>(),
                .document_id = r["document_id"].as<std::string>(),
                .acl_version = r["acl_version"].as<std::int64_t>(),
            });
        }
    } catch (const drogon::orm::DrogonDbException& ex) {
        spdlog::warn("[resync-worker] claim_batch failed: {}", ex.base().what());
    }
    co_return events;
}

drogon::Task<int> ResyncWorker::reap_stale_claims()
{
    try {
        auto rows = co_await db_->execSqlCoro(std::format(R"(
            UPDATE outbox_events
            SET    claimed_at    = NULL,
                   claimed_by    = NULL,
                   attempt_count = GREATEST(attempt_count - 1, 0),
                   -- Bound last_error to 4KB so repeated reap cycles do
                   -- not accumulate a multi-megabyte tail across retries.
                   last_error    = left(
                       COALESCE(last_error, '') ||
                       ' [reaped: stale claim by ' ||
                       COALESCE(claimed_by, '?') || ']',
                       4096)
            WHERE  job_type     = 'qdrant_resync_chunk_acl'
              AND  completed_at IS NULL
              AND  claimed_at IS NOT NULL
              AND  claimed_at < now() - interval '{} minutes'
            RETURNING id::text
        )", opts_.claim_lease.count()));
        const int n = static_cast<int>(rows.size());
        for (const auto& r : rows)
            spdlog::warn("[resync-worker] reaped stale claim on event {} (age > {} min)",
                         r["id"].as<std::string>(), opts_.claim_lease.count());
        co_return n;
    } catch (const drogon::orm::DrogonDbException& ex) {
        spdlog::warn("[resync-worker] reap_stale_claims failed: {}", ex.base().what());
        co_return 0;
    }
}

drogon::Task<int> ResyncWorker::release_my_claims()
{
    try {
        auto rows = co_await db_->execSqlCoro(R"(
            UPDATE outbox_events
            SET    claimed_at    = NULL,
                   claimed_by    = NULL,
                   attempt_count = GREATEST(attempt_count - 1, 0)
            WHERE  job_type     = 'qdrant_resync_chunk_acl'
              AND  completed_at IS NULL
              AND  claimed_by   = $1
            RETURNING id::text
        )", worker_id_);
        const int n = static_cast<int>(rows.size());
        if (n > 0)
            spdlog::info("[resync-worker] {} released {} unprocessed claim(s) on shutdown",
                         worker_id_, n);
        co_return n;
    } catch (const drogon::orm::DrogonDbException& ex) {
        spdlog::warn("[resync-worker] release_my_claims failed: {}", ex.base().what());
        co_return 0;
    }
}

// ---------------------------------------------------------------------------
// Process one claimed resync event. See the header for the CAS protocol.
// ---------------------------------------------------------------------------

drogon::Task<Result<ResyncWorker::Outcome>>
ResyncWorker::process(const ClaimedEvent& ev)
{
    // Everything runs inside ONE transaction that first takes a per-document
    // advisory xact lock, so no two resync workers can interleave their
    // read->write->CAS for the same document (design 2.3). The lock (and the
    // pooled connection) is held across the Qdrant network write; resync is a
    // low-rate path and drain_once processes events sequentially, so at most
    // one connection is held per worker at a time.
    std::optional<postgres::UnitOfWork> uow_opt;
    try {
        uow_opt.emplace(co_await postgres::UnitOfWork::begin(db_));
    } catch (const std::exception& ex) {
        co_return std::unexpected(Error::unavailable(
            std::string("resync: begin tx failed: ") + ex.what()));
    }
    auto& uow = *uow_opt;

    // 1. Serialize on the document, then read its current ACL truth.
    std::string  owner_org_unit_id;
    std::int64_t v_cur = 0;
    try {
        // Blocking lock: a competing worker for the same document waits here
        // until we commit, guaranteeing our Qdrant write + CAS are atomic
        // w.r.t. any other resync of this document.
        co_await uow.exec("SELECT pg_advisory_xact_lock(hashtext($1))",
                          "resync:doc:" + ev.document_id);
        auto rows = co_await uow.exec(
            "SELECT owner_org_unit_id::text AS owner, acl_version "
            "FROM documents WHERE company_id = $1::uuid AND id = $2::uuid",
            ev.company_id, ev.document_id);
        if (rows.empty()) {
            // Document deleted since the event was enqueued; its points are
            // cascade-removed independently. Nothing to resync -> drop.
            uow.rollback();
            spdlog::info("[resync-worker] event={} document={} gone; dropping",
                         ev.id, ev.document_id);
            co_return Outcome::Superseded;
        }
        owner_org_unit_id = rows[0]["owner"].as<std::string>();
        v_cur             = rows[0]["acl_version"].as<std::int64_t>();
    } catch (const drogon::orm::DrogonDbException& ex) {
        uow.rollback();
        co_return std::unexpected(postgres::map_db_exception(ex));
    }

    // 2. Out-of-order drop: a strictly newer ACL bump has already committed and
    //    enqueued its own event, which will do the authoritative write.
    if (ev.acl_version < v_cur) {
        uow.rollback();
        spdlog::info("[resync-worker] event={} document={} superseded "
                     "(event acl_version={} < current {}); dropping",
                     ev.id, ev.document_id, ev.acl_version, v_cur);
        co_return Outcome::Superseded;
    }

    // 3. Recompute the resource-axis scope LIVE from resource_grants (the whole
    //    point of the resync: the denormalized column is stale). Read-only, so
    //    the separate connection used by the repo does not extend the lock.
    auto scope_r = co_await repo_->fetch_access_scopes(ev.company_id, ev.document_id);
    if (!scope_r) {
        uow.rollback();
        co_return std::unexpected(scope_r.error());
    }
    const auto& scope = *scope_r;

    // 4. Collect the active version's point ids GROUPED BY Qdrant collection
    //    (a chunk may be embedded by several models, each in its own
    //    collection), plus the version's uniform sensitivity / lifecycle.
    std::map<std::string, std::vector<std::string>> points_by_collection;
    std::string sensitivity = "internal";
    std::string lifecycle   = "active";
    try {
        auto rows = co_await uow.exec(R"(
            SELECT dcv.qdrant_point_id::text AS point_id,
                   em.qdrant_collection      AS collection,
                   dv.sensitivity_label      AS sensitivity_label,
                   dv.lifecycle_status       AS lifecycle_status
            FROM   document_versions dv
            JOIN   document_chunks dc
                ON dc.document_version_id = dv.id AND dc.company_id = dv.company_id
            JOIN   document_chunk_vectors dcv
                ON dcv.chunk_id = dc.id AND dcv.company_id = dc.company_id
            JOIN   embedding_models em
                ON em.id = dcv.embedding_model_id
            WHERE  dv.company_id = $1::uuid
              AND  dv.document_id = $2::uuid
              AND  dv.lifecycle_status = 'active'
        )", ev.company_id, ev.document_id);
        for (const auto& r : rows) {
            points_by_collection[r["collection"].as<std::string>()]
                .push_back(r["point_id"].as<std::string>());
            sensitivity = r["sensitivity_label"].as<std::string>();
            lifecycle   = r["lifecycle_status"].as<std::string>();
        }
    } catch (const drogon::orm::DrogonDbException& ex) {
        uow.rollback();
        co_return std::unexpected(postgres::map_db_exception(ex));
    }

    // 5. Rewrite the Qdrant payload keys in EVERY collection (no re-embedding).
    //    A collection this deployment cannot serve is a hard error: advancing
    //    synced_version while leaving it stale would be a silent lie.
    rag::PayloadPatch patch{
        .access_scope_ids  = scope,
        .sensitivity_label = sensitivity,
        .lifecycle_status  = lifecycle,
        .owner_org_unit_id = owner_org_unit_id,
        .acl_version       = v_cur,
    };
    for (const auto& [collection, ids] : points_by_collection) {
        auto store = store_for_collection_(collection);
        if (!store) {
            uow.rollback();
            co_return std::unexpected(Error::invalid_state(std::format(
                "resync: no vector store bound to Qdrant collection '{}' "
                "(document {} has points there); route a worker for it",
                collection, ev.document_id)));
        }
        if (auto r = co_await store->set_payload(ev.company_id, ids, patch); !r) {
            uow.rollback();
            co_return std::unexpected(r.error());
        }
    }

    // 6. Refresh the denormalized column for EVERY chunk of the document so a
    //    later re-ingest/upsert writes the correct scope. Scope is
    //    document-level (owner + grants), identical across all versions.
    try {
        co_await uow.exec(R"(
            UPDATE document_chunks dc
            SET    qdrant_prefilter_scope_ids = $3::uuid[]
            FROM   document_versions dv
            WHERE  dv.company_id  = $1::uuid
              AND  dv.document_id = $2::uuid
              AND  dc.document_version_id = dv.id
              AND  dc.company_id  = $1::uuid
        )", ev.company_id, ev.document_id, to_pg_uuid_array(scope));
    } catch (const drogon::orm::DrogonDbException& ex) {
        uow.rollback();
        co_return std::unexpected(postgres::map_db_exception(ex));
    }

    // 7. Compare-and-set the monotonic synced_version. Zero rows means a
    //    concurrent grant-mutation bump moved acl_version during processing;
    //    its own event (enqueued by the bump) re-writes under this same lock.
    Outcome outcome = Outcome::Completed;
    try {
        auto rows = co_await uow.exec(R"(
            UPDATE documents
            SET    qdrant_synced_version = $2::bigint
            WHERE  id          = $1::uuid
              AND  company_id  = $3::uuid
              AND  acl_version = $2::bigint
            RETURNING id::text
        )", ev.document_id, v_cur, ev.company_id);
        if (rows.empty()) {
            spdlog::info("[resync-worker] event={} document={} raced (acl_version "
                         "moved past {} during processing); newer event owns it",
                         ev.id, ev.document_id, v_cur);
            outcome = Outcome::Superseded;
        }
    } catch (const drogon::orm::DrogonDbException& ex) {
        uow.rollback();
        co_return std::unexpected(postgres::map_db_exception(ex));
    }

    // Commit the payload refresh (column) + CAS atomically, releasing the
    // advisory lock. A commit failure fails the event so the next poll retries
    // (set_payload is idempotent).
    if (auto r = co_await uow.commit(); !r)
        co_return std::unexpected(r.error());

    co_return outcome;
}

drogon::Task<void> ResyncWorker::mark_completed(const std::string& event_id)
{
    try {
        co_await db_->execSqlCoro(
            "UPDATE outbox_events SET completed_at = now(), claimed_at = NULL "
            "WHERE id = $1::uuid",
            event_id);
    } catch (const drogon::orm::DrogonDbException& ex) {
        spdlog::error("[resync-worker] mark_completed({}) failed: {}",
                      event_id, ex.base().what());
    }
}

drogon::Task<void> ResyncWorker::mark_failed(const std::string& event_id,
                                             std::string_view  reason)
{
    try {
        // Exponential backoff (2,4,8,...,capped 300s), matching OutboxWorker so
        // a transient Qdrant/PG blip does not burn the retry budget in seconds.
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
        spdlog::error("[resync-worker] mark_failed({}) failed: {}",
                      event_id, ex.base().what());
    }
}

drogon::Task<int> ResyncWorker::drain_once()
{
    auto batch = co_await claim_batch();
    int processed = 0;
    for (const auto& ev : batch) {
        if (shutdown_()) break;
        auto r = co_await process(ev);
        if (r) {
            co_await mark_completed(ev.id);
            if (*r == Outcome::Superseded)
                events_superseded_.fetch_add(1);
            else
                events_completed_.fetch_add(1);
            ++processed;
        } else {
            const auto& err = r.error();
            co_await mark_failed(ev.id, err.message);
            events_failed_.fetch_add(1);
            spdlog::warn("[resync-worker] event={} document={} failed: {}",
                         ev.id, ev.document_id, err.message);
        }
    }
    co_return processed;
}

drogon::Task<void> ResyncWorker::run()
{
    spdlog::info("[resync-worker] {} starting; poll={}ms batch_size={} lease={}min",
                 worker_id_, opts_.poll_interval.count(), opts_.batch_size,
                 opts_.claim_lease.count());
    while (!shutdown_()) {
        int processed = co_await drain_once();
        if (processed < opts_.batch_size)
            co_await co_sleep(opts_.poll_interval);
    }
    co_await release_my_claims();
    spdlog::info("[resync-worker] {} drained; exiting", worker_id_);
}

} // namespace wikore::scheduler
