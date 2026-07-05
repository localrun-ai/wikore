#include "wikore/scheduler/embed_edge_worker.hpp"
#include "wikore/adapters/postgres/error_mapper.hpp"
#include "wikore/domain/knowledge_edge.hpp"
#include "wikore/rag/edge_vector_formula.hpp"
#include "wikore/rag/knowledge_edge_descriptions.hpp"
#include <drogon/drogon.h>
#include <openssl/evp.h>
#include <spdlog/spdlog.h>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <unistd.h>

namespace wikore::scheduler {

namespace {

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

std::string bytes_to_uuid(const std::array<std::uint8_t, 16>& b)
{
    char buf[37];
    std::snprintf(buf, sizeof(buf),
                  "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                  b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
                  b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return std::string{buf};
}

// RFC 4122 v5 UUID: SHA-1(namespace || name), then set version=5
// (nibble 12) and variant=10 (top two bits of byte 8). Deterministic —
// same inputs always produce the same output.
// Namespace: DNS namespace 6ba7b810-9dad-11d1-80b4-00c04fd430c8 (the
// canonical RFC 4122 DNS namespace). Consistent with what Postgres's
// uuid_generate_v5(uuid_ns_dns(), ...) would return, so the V037
// header comment stays honest.
std::string uuid_v5_dns(std::string_view name)
{
    constexpr std::array<std::uint8_t, 16> kDnsNs = {
        0x6b, 0xa7, 0xb8, 0x10, 0x9d, 0xad, 0x11, 0xd1,
        0x80, 0xb4, 0x00, 0xc0, 0x4f, 0xd4, 0x30, 0xc8
    };
    // OpenSSL 3 EVP API (SHA1_Init/Update/Final are deprecated).
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int  hash_len = 0;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha1(), nullptr);
    EVP_DigestUpdate(ctx, kDnsNs.data(), kDnsNs.size());
    EVP_DigestUpdate(ctx, name.data(), name.size());
    EVP_DigestFinal_ex(ctx, hash, &hash_len);
    EVP_MD_CTX_free(ctx);
    std::array<std::uint8_t, 16> out{};
    std::memcpy(out.data(), hash, 16);
    out[6] = static_cast<std::uint8_t>((out[6] & 0x0F) | 0x50);   // version = 5
    out[8] = static_cast<std::uint8_t>((out[8] & 0x3F) | 0x80);   // RFC 4122 variant
    return bytes_to_uuid(out);
}

// V037 5b-contract point-id formula (version-free):
//   uuid_v5(DNS, edge_id + ':' + embedding_model_id + ':' + formula_version)
std::string compute_point_id(std::string_view edge_id,
                             std::string_view embedding_model_id,
                             int              formula_version)
{
    std::string name = std::format("{}:{}:{}",
                                   edge_id, embedding_model_id, formula_version);
    return uuid_v5_dns(name);
}

} // namespace

EmbedEdgeWorker::EmbedEdgeWorker(
    drogon::orm::DbClientPtr                db,
    VectorStoreForCollection                store_for_collection,
    std::shared_ptr<rag::VectorStorePort>   edge_store,
    std::string                             edge_collection,
    ShutdownPredicate                       shutdown_requested,
    Options                                 opts)
    : db_(std::move(db))
    , store_for_collection_(std::move(store_for_collection))
    , edge_store_(std::move(edge_store))
    , edge_collection_(std::move(edge_collection))
    , shutdown_(std::move(shutdown_requested))
    , opts_(std::move(opts))
{
    char host[256] = {0};
    gethostname(host, sizeof(host) - 1);
    worker_id_ = std::format("{}:{}", host, getpid());
}

drogon::Task<std::vector<EmbedEdgeWorker::ClaimedEvent>>
EmbedEdgeWorker::claim_batch()
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
            WHERE  job_type        = 'qdrant_upsert_edge_vector'
              AND  completed_at    IS NULL
              AND  claimed_at      IS NULL
              AND  attempt_count   < $2::int
              AND  next_attempt_at <= now()
            ORDER BY next_attempt_at
            FOR UPDATE SKIP LOCKED
            LIMIT $1::int
        )
        RETURNING
               e.id::text                                     AS id,
               e.company_id::text                             AS company_id,
               e.aggregate_id::text                           AS edge_id,
               COALESCE(e.payload->>'edge_type', '')          AS edge_type,
               COALESCE(e.payload->>'embedding_model_id', '') AS embedding_model_id,
               COALESCE((e.payload->>'formula_version')::int, 0)    AS formula_version,
               COALESCE((e.payload->>'edge_version')::bigint, 0)    AS edge_version,
               COALESCE(e.payload->>'review_state', '')       AS review_state
    )";

    std::vector<ClaimedEvent> events;
    try {
        auto rows = co_await db_->execSqlCoro(kSql,
            opts_.batch_size, opts_.max_attempts, worker_id_);
        events.reserve(rows.size());
        for (const auto& r : rows) {
            events.push_back(ClaimedEvent{
                .id                 = r["id"].as<std::string>(),
                .company_id         = r["company_id"].as<std::string>(),
                .edge_id            = r["edge_id"].as<std::string>(),
                .edge_type          = r["edge_type"].as<std::string>(),
                .embedding_model_id = r["embedding_model_id"].as<std::string>(),
                .formula_version    = r["formula_version"].as<int>(),
                .edge_version       = r["edge_version"].as<std::int64_t>(),
                .review_state       = r["review_state"].as<std::string>(),
            });
        }
    } catch (const drogon::orm::DrogonDbException& ex) {
        spdlog::warn("[embed-edge-worker] claim_batch failed: {}", ex.base().what());
    }
    co_return events;
}

drogon::Task<int> EmbedEdgeWorker::reap_stale_claims()
{
    try {
        auto rows = co_await db_->execSqlCoro(std::format(R"(
            UPDATE outbox_events
            SET    claimed_at    = NULL,
                   claimed_by    = NULL,
                   attempt_count = GREATEST(attempt_count - 1, 0),
                   -- Bound last_error to 4KB (same reaper pattern as
                   -- ResyncWorker / EdgeVectorCleanupWorker).
                   last_error    = left(
                       COALESCE(last_error, '') ||
                       ' [reaped: stale claim by ' ||
                       COALESCE(claimed_by, '?') || ']',
                       4096)
            WHERE  job_type     = 'qdrant_upsert_edge_vector'
              AND  completed_at IS NULL
              AND  claimed_at IS NOT NULL
              AND  claimed_at < now() - interval '{} minutes'
            RETURNING id::text
        )", opts_.claim_lease.count()));
        co_return static_cast<int>(rows.size());
    } catch (const drogon::orm::DrogonDbException& ex) {
        spdlog::warn("[embed-edge-worker] reap_stale_claims failed: {}", ex.base().what());
        co_return 0;
    }
}

drogon::Task<int> EmbedEdgeWorker::release_my_claims()
{
    try {
        auto rows = co_await db_->execSqlCoro(R"(
            UPDATE outbox_events
            SET    claimed_at    = NULL,
                   claimed_by    = NULL,
                   attempt_count = GREATEST(attempt_count - 1, 0)
            WHERE  job_type     = 'qdrant_upsert_edge_vector'
              AND  completed_at IS NULL
              AND  claimed_by   = $1
            RETURNING id::text
        )", worker_id_);
        const int n = static_cast<int>(rows.size());
        if (n > 0)
            spdlog::info("[embed-edge-worker] {} released {} unprocessed claim(s) on shutdown",
                         worker_id_, n);
        co_return n;
    } catch (const drogon::orm::DrogonDbException& ex) {
        spdlog::warn("[embed-edge-worker] release_my_claims failed: {}", ex.base().what());
        co_return 0;
    }
}

// ---------------------------------------------------------------------------
// Helpers split out of process() so each coroutine frame stays small.
// GCC 14 ICEs (build_special_member_call at cp/call.cc:11096) on the
// original monolithic process() — same class of bug the
// KnowledgeEdgeRepo::get()/list() workaround comments in #54 document.
// Each helper does exactly one DB or Qdrant round-trip, keeps the
// try/catch tight around it, and returns a Result the orchestrator
// consumes at the outermost frame.
// ---------------------------------------------------------------------------

drogon::Task<Result<std::optional<EmbedEdgeWorker::LiveEdge>>>
EmbedEdgeWorker::load_live_edge(const ClaimedEvent& ev)
{
    // Read the live edge state — critically INCLUDING edge_type,
    // review_state, and edge_version — so the payload we write to
    // Qdrant reflects newest-known truth, not the (possibly stale)
    // event payload. Without this, a v2 event processed after v3
    // could overwrite the point with v2's review_state, over-recalling
    // rejected edges past when the retrieval prefilter starts trusting
    // this key.
    drogon::orm::Result rows{nullptr};
    try {
        rows = co_await db_->execSqlCoro(R"(
            SELECT e.edge_type::text                     AS edge_type,
                   e.review_state::text                  AS review_state,
                   e.edge_version::bigint                AS edge_version,
                   ep0.chunk_id::text                    AS ep0_chunk_id,
                   ep1.chunk_id::text                    AS ep1_chunk_id,
                   COALESCE(d0.authority_level, 50)      AS auth0,
                   COALESCE(d1.authority_level, 50)      AS auth1
            FROM knowledge_edges e
            JOIN knowledge_edge_endpoints ep0
              ON ep0.edge_id = e.id AND ep0.ordinal = 0
            JOIN knowledge_edge_endpoints ep1
              ON ep1.edge_id = e.id AND ep1.ordinal = 1
            JOIN document_chunks    c0 ON c0.id = ep0.chunk_id
            JOIN document_chunks    c1 ON c1.id = ep1.chunk_id
            JOIN document_versions  v0 ON v0.id = c0.document_version_id
            JOIN document_versions  v1 ON v1.id = c1.document_version_id
            JOIN documents          d0 ON d0.id = v0.document_id
            JOIN documents          d1 ON d1.id = v1.document_id
            WHERE e.id = $1::uuid AND e.company_id = $2::uuid
        )", ev.edge_id, ev.company_id);
    } catch (const drogon::orm::DrogonDbException& ex) {
        co_return std::unexpected(postgres::map_db_exception(ex));
    }
    if (rows.empty())
        co_return std::optional<LiveEdge>{};
    co_return std::optional<LiveEdge>{LiveEdge{
        .edge_type    = rows[0]["edge_type"].as<std::string>(),
        .review_state = rows[0]["review_state"].as<std::string>(),
        .edge_version = rows[0]["edge_version"].as<std::int64_t>(),
        .ep0_chunk_id = rows[0]["ep0_chunk_id"].as<std::string>(),
        .ep1_chunk_id = rows[0]["ep1_chunk_id"].as<std::string>(),
        .auth0        = rows[0]["auth0"].as<int>(),
        .auth1        = rows[0]["auth1"].as<int>(),
    }};
}

drogon::Task<Result<std::int64_t>>
EmbedEdgeWorker::load_indexed_edge_version(const ClaimedEvent& ev)
{
    drogon::orm::Result rows{nullptr};
    try {
        rows = co_await db_->execSqlCoro(
            "SELECT indexed_edge_version::bigint AS iev "
            "FROM knowledge_edge_embeddings "
            "WHERE edge_id = $1::uuid AND embedding_model_id = $2::uuid",
            ev.edge_id, ev.embedding_model_id);
    } catch (const drogon::orm::DrogonDbException& ex) {
        co_return std::unexpected(postgres::map_db_exception(ex));
    }
    if (rows.empty()) co_return static_cast<std::int64_t>(-1);
    co_return rows[0]["iev"].as<std::int64_t>();
}

// Returns std::nullopt when the model is disabled — the caller treats
// that as ModelDisabled (complete-and-no-op), not as a failure to
// retry. A disable is a legitimate operator action; burning the retry
// budget on it would produce alert noise.
drogon::Task<Result<std::optional<std::string>>>
EmbedEdgeWorker::load_model_collection(const std::string& model_id)
{
    drogon::orm::Result rows{nullptr};
    try {
        rows = co_await db_->execSqlCoro(
            "SELECT qdrant_collection FROM embedding_models "
            "WHERE id = $1::uuid AND enabled = true",
            model_id);
    } catch (const drogon::orm::DrogonDbException& ex) {
        co_return std::unexpected(postgres::map_db_exception(ex));
    }
    if (rows.empty())
        co_return std::optional<std::string>{};
    co_return std::optional<std::string>{rows[0]["qdrant_collection"].as<std::string>()};
}

drogon::Task<Result<std::pair<std::string, std::string>>>
EmbedEdgeWorker::load_endpoint_point_ids(const ClaimedEvent& ev, const LiveEdge& edge)
{
    // ANY($1::uuid[]) expects '{a,b}' text; the chunk ids came out of
    // Postgres so they are already valid UUID text.
    std::string arr = std::format("{{{},{}}}", edge.ep0_chunk_id, edge.ep1_chunk_id);
    std::string mid = ev.embedding_model_id;
    drogon::orm::Result rows{nullptr};
    try {
        rows = co_await db_->execSqlCoro(
            "SELECT chunk_id::text AS chunk_id, qdrant_point_id::text AS pid "
            "FROM document_chunk_vectors "
            "WHERE chunk_id = ANY($1::uuid[]) AND embedding_model_id = $2::uuid",
            arr, mid);
    } catch (const drogon::orm::DrogonDbException& ex) {
        co_return std::unexpected(postgres::map_db_exception(ex));
    }
    std::string ep0_point_id, ep1_point_id;
    for (const auto& r : rows) {
        const auto cid = r["chunk_id"].as<std::string>();
        const auto pid = r["pid"].as<std::string>();
        if (cid == edge.ep0_chunk_id) ep0_point_id = pid;
        if (cid == edge.ep1_chunk_id) ep1_point_id = pid;
    }
    if (ep0_point_id.empty() || ep1_point_id.empty())
        co_return std::unexpected(Error::unavailable(std::format(
            "endpoint chunk vectors not yet embedded for model {} "
            "(ep0='{}' ep1='{}'); retry",
            ev.embedding_model_id, ep0_point_id, ep1_point_id)));
    co_return std::pair{ep0_point_id, ep1_point_id};
}

drogon::Task<Result<rag::Embedding>>
EmbedEdgeWorker::load_type_vector(const ClaimedEvent& ev)
{
    std::string type_arg{ev.edge_type};
    std::string mid_arg{ev.embedding_model_id};
    drogon::orm::Result rows{nullptr};
    try {
        rows = co_await db_->execSqlCoro(
            "SELECT vector FROM edge_type_vectors "
            "WHERE formula_version = $1::int AND edge_type = $2::text "
            "  AND embedding_model_id = $3::uuid",
            ev.formula_version, type_arg, mid_arg);
    } catch (const drogon::orm::DrogonDbException& ex) {
        co_return std::unexpected(postgres::map_db_exception(ex));
    }
    if (rows.empty())
        co_return std::unexpected(Error::unavailable(std::format(
            "edge_type_vectors row missing for (formula_version={}, "
            "edge_type='{}', model={}); run EmbedTypeVectorsUseCase",
            ev.formula_version, ev.edge_type, ev.embedding_model_id)));
    // Postgres text-protocol for real[] arrives as '{a,b,c}'. Parse
    // once and return.
    rag::Embedding v_type;
    try {
        const std::string s = rows[0]["vector"].as<std::string>();
        std::string body = s.substr(1, s.size() - 2);
        std::string tok;
        for (char c : body) {
            if (c == ',') {
                if (!tok.empty()) v_type.push_back(std::stof(tok));
                tok.clear();
            } else {
                tok.push_back(c);
            }
        }
        if (!tok.empty()) v_type.push_back(std::stof(tok));
    } catch (const std::exception& ex) {
        co_return std::unexpected(Error::database_error(std::format(
            "edge_type_vectors vector parse error: {}", ex.what())));
    }
    co_return v_type;
}

drogon::Task<Result<void>>
EmbedEdgeWorker::upsert_bookkeeping(const ClaimedEvent& ev,
                                    const std::string&  qdrant_point_id,
                                    std::int64_t        edge_version)
{
    std::string cid_arg{ev.company_id};
    std::string eid_arg{ev.edge_id};
    std::string mid_arg{ev.embedding_model_id};
    std::string pid_arg = qdrant_point_id;
    try {
        co_await db_->execSqlCoro(
            "INSERT INTO knowledge_edge_embeddings "
            "  (company_id, edge_id, embedding_model_id, qdrant_point_id, "
            "   formula_version, indexed_edge_version) "
            "VALUES ($1::uuid, $2::uuid, $3::uuid, $4::uuid, $5::int, $6::bigint) "
            "ON CONFLICT (edge_id, embedding_model_id) DO UPDATE SET "
            "  qdrant_point_id      = EXCLUDED.qdrant_point_id, "
            "  formula_version      = EXCLUDED.formula_version, "
            "  indexed_edge_version = EXCLUDED.indexed_edge_version, "
            "  indexed_at           = now() "
            "WHERE knowledge_edge_embeddings.indexed_edge_version "
            "    < EXCLUDED.indexed_edge_version",
            cid_arg, eid_arg, mid_arg, pid_arg,
            ev.formula_version, edge_version);
    } catch (const drogon::orm::DrogonDbException& ex) {
        co_return std::unexpected(postgres::map_db_exception(ex));
    }
    co_return Result<void>{};
}

// ---------------------------------------------------------------------------
// Process one claimed event. Implements the V037 5b-contract:
//   * skip-if-gone → Outcome::NoEdge, event marked completed;
//   * disabled model → Outcome::ModelDisabled, event marked completed;
//   * CAS on knowledge_edge_embeddings.indexed_edge_version →
//     Outcome::Superseded when live edge_version <= stored;
//   * otherwise compute formula v1 and upsert to Qdrant, then
//     UPSERT the bookkeeping row.
//
// The orchestrator is a flat sequence of co_awaits with NO try/catch
// around them — each helper has its own try/catch tight around one
// DB round-trip and returns a Result. Same shape as
// KnowledgeEdgeRepo::get()/list() after PR #54's GCC 14 fix.
// ---------------------------------------------------------------------------

drogon::Task<Result<EmbedEdgeWorker::Outcome>>
EmbedEdgeWorker::process(const ClaimedEvent& ev)
{
    // 1. Live-edge probe.
    auto live_opt = co_await load_live_edge(ev);
    if (!live_opt) co_return std::unexpected(live_opt.error());
    if (!live_opt->has_value())
        co_return Outcome::NoEdge;
    const LiveEdge& live = **live_opt;

    // 2. Staleness gate — event's edge_version vs. stored
    //    indexed_edge_version. Live edge_version is the tie-breaker
    //    for concurrent workers: even if a stale (older) event wins
    //    the claim, it observes the newer indexed_edge_version and
    //    superseeds itself.
    auto iev = co_await load_indexed_edge_version(ev);
    if (!iev) co_return std::unexpected(iev.error());
    // A stale event whose payload version is behind the live edge is
    // still worth processing IF nothing has been indexed yet — that
    // handles the very first upsert after a burst of enqueue events.
    // But if the DB already has a newer indexed_edge_version, we are
    // late; drop.
    if (*iev >= live.edge_version)
        co_return Outcome::Superseded;

    // 3. Model collection lookup + disabled-model treatment.
    auto coll_opt = co_await load_model_collection(ev.embedding_model_id);
    if (!coll_opt) co_return std::unexpected(coll_opt.error());
    if (!coll_opt->has_value())
        // Model disabled between enqueue and claim — legitimate
        // operator action, not something to retry. Complete-and-no-op.
        co_return Outcome::ModelDisabled;
    const std::string collection = **coll_opt;
    auto chunk_store = store_for_collection_(collection);
    if (!chunk_store)
        co_return std::unexpected(Error::invalid_state(std::format(
            "no vector store bound to collection '{}' — route a worker for it",
            collection)));

    // 4. Endpoint point-id lookup + vector fetch from Qdrant.
    auto point_ids = co_await load_endpoint_point_ids(ev, live);
    if (!point_ids) co_return std::unexpected(point_ids.error());
    const auto& [ep0_pid, ep1_pid] = *point_ids;
    auto vecs = co_await chunk_store->fetch_vectors_by_id({ep0_pid, ep1_pid});
    if (!vecs) co_return std::unexpected(vecs.error());
    const rag::Embedding* v0 = nullptr;
    const rag::Embedding* v1 = nullptr;
    for (const auto& [pid, vec] : *vecs) {
        if (pid == ep0_pid) v0 = &vec;
        if (pid == ep1_pid) v1 = &vec;
    }
    if (!v0 || !v1)
        co_return std::unexpected(Error::unavailable(std::format(
            "Qdrant did not return one of the endpoint vectors "
            "(ep0='{}' ep1='{}'); retry", ep0_pid, ep1_pid)));

    // 5. Type vector.
    auto v_type = co_await load_type_vector(ev);
    if (!v_type) co_return std::unexpected(v_type.error());

    // 6. Formula v1.
    auto edge_vec = rag::compute_edge_vector_v1(live.auth0, live.auth1,
                                                *v0, *v1, *v_type);
    if (!edge_vec) co_return std::unexpected(edge_vec.error());

    // 7. Version-free point id.
    const std::string pid = compute_point_id(ev.edge_id,
                                             ev.embedding_model_id,
                                             ev.formula_version);

    // 8. Qdrant upsert.
    //
    // Every payload field the retrieval prefilter might key on
    // (review_state, edge_version, edge_type, endpoints, authorities)
    // is taken from the LIVE edge, not the event payload. A late-
    // arriving stale event would still refuse the write above via the
    // indexed_edge_version CAS, but even if that check missed, the
    // payload it writes here matches current truth.
    std::string payload_json = std::format(
        R"({{"company_id":"{}","edge_id":"{}","edge_type":"{}",)"
        R"("formula_version":{},"edge_version":{},)"
        R"("endpoint_0_chunk_id":"{}","endpoint_1_chunk_id":"{}",)"
        R"("authority_0":{},"authority_1":{},"review_state":"{}"}})",
        ev.company_id, ev.edge_id, live.edge_type,
        ev.formula_version, live.edge_version,
        live.ep0_chunk_id, live.ep1_chunk_id,
        live.auth0, live.auth1, live.review_state);
    if (auto r = co_await edge_store_->upsert_raw(pid, *edge_vec, std::move(payload_json)); !r)
        co_return std::unexpected(r.error());

    // 9. Bookkeeping — use live.edge_version, not ev.edge_version, so
    //    the row we advertise as indexed matches what we actually
    //    wrote to Qdrant.
    if (auto r = co_await upsert_bookkeeping(ev, pid, live.edge_version); !r)
        co_return std::unexpected(r.error());

    co_return Outcome::Completed;
}

drogon::Task<void>
EmbedEdgeWorker::mark_completed(const std::string& event_id)
{
    try {
        co_await db_->execSqlCoro(
            "UPDATE outbox_events SET completed_at = now(), claimed_at = NULL "
            "WHERE id = $1::uuid", event_id);
    } catch (const drogon::orm::DrogonDbException& ex) {
        spdlog::error("[embed-edge-worker] mark_completed({}) failed: {}",
                      event_id, ex.base().what());
    }
}

drogon::Task<void>
EmbedEdgeWorker::mark_failed(const std::string& event_id, std::string_view reason)
{
    try {
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
        spdlog::error("[embed-edge-worker] mark_failed({}) failed: {}",
                      event_id, ex.base().what());
    }
}

drogon::Task<int> EmbedEdgeWorker::drain_once()
{
    auto batch = co_await claim_batch();
    int processed = 0;
    for (const auto& ev : batch) {
        if (shutdown_()) break;
        auto r = co_await process(ev);
        if (r) {
            co_await mark_completed(ev.id);
            switch (*r) {
                case Outcome::Completed:     events_completed_.fetch_add(1);      break;
                case Outcome::Superseded:    events_superseded_.fetch_add(1);     break;
                case Outcome::NoEdge:        events_no_edge_.fetch_add(1);        break;
                case Outcome::ModelDisabled: events_model_disabled_.fetch_add(1); break;
            }
            ++processed;
        } else {
            co_await mark_failed(ev.id, r.error().message);
            events_failed_.fetch_add(1);
            spdlog::warn("[embed-edge-worker] event={} edge={} model={} failed: {}",
                         ev.id, ev.edge_id, ev.embedding_model_id, r.error().message);
        }
    }
    co_return processed;
}

drogon::Task<void> EmbedEdgeWorker::run()
{
    spdlog::info("[embed-edge-worker] {} starting; poll={}ms batch_size={} "
                 "edge_collection={}",
                 worker_id_, opts_.poll_interval.count(), opts_.batch_size,
                 edge_collection_);
    while (!shutdown_()) {
        int processed = co_await drain_once();
        if (processed < opts_.batch_size)
            co_await co_sleep(opts_.poll_interval);
    }
    co_await release_my_claims();
    spdlog::info("[embed-edge-worker] {} drained; exiting", worker_id_);
}

} // namespace wikore::scheduler
