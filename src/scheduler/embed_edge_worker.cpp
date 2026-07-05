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
#include <memory>
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
// Helpers — every one returns Result<void> and writes results through
// out-params. Return types like Result<optional<T>> or Result<pair<...>>
// trip GCC 14's coroutine-frame emitter (ICE: build_special_member_call
// at cp/call.cc:11096); Result<void> keeps each helper frame minimal.
// ---------------------------------------------------------------------------

drogon::Task<Result<void>>
EmbedEdgeWorker::load_live_edge(const ClaimedEvent& ev, LiveEdge& out)
{
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
    if (rows.empty()) {
        out.missing = true;
        co_return Result<void>{};
    }
    out.missing      = false;
    out.edge_type    = rows[0]["edge_type"].as<std::string>();
    out.review_state = rows[0]["review_state"].as<std::string>();
    out.edge_version = rows[0]["edge_version"].as<std::int64_t>();
    out.ep0_chunk_id = rows[0]["ep0_chunk_id"].as<std::string>();
    out.ep1_chunk_id = rows[0]["ep1_chunk_id"].as<std::string>();
    out.auth0        = rows[0]["auth0"].as<int>();
    out.auth1        = rows[0]["auth1"].as<int>();
    co_return Result<void>{};
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

drogon::Task<Result<void>>
EmbedEdgeWorker::load_model_collection(const std::string& model_id,
                                       std::string&       out_collection)
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
    // Empty out means model exists but is disabled (or the model row
    // is absent altogether). The caller distinguishes based on caller
    // context — for the worker, either case is Outcome::ModelDisabled
    // (both are legitimate operator actions).
    if (rows.empty())
        out_collection.clear();
    else
        out_collection = rows[0]["qdrant_collection"].as<std::string>();
    co_return Result<void>{};
}

drogon::Task<Result<void>>
EmbedEdgeWorker::load_endpoint_point_ids(const ClaimedEvent& ev,
                                         const LiveEdge&     edge,
                                         std::string&        out_ep0_pid,
                                         std::string&        out_ep1_pid)
{
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
    for (const auto& r : rows) {
        const auto cid = r["chunk_id"].as<std::string>();
        const auto pid = r["pid"].as<std::string>();
        if (cid == edge.ep0_chunk_id) out_ep0_pid = pid;
        if (cid == edge.ep1_chunk_id) out_ep1_pid = pid;
    }
    if (out_ep0_pid.empty() || out_ep1_pid.empty())
        co_return std::unexpected(Error::unavailable(std::format(
            "endpoint chunk vectors not yet embedded for model {} "
            "(ep0='{}' ep1='{}'); retry",
            ev.embedding_model_id, out_ep0_pid, out_ep1_pid)));
    co_return Result<void>{};
}

drogon::Task<Result<void>>
EmbedEdgeWorker::load_type_vector(const ClaimedEvent& ev, rag::Embedding& out_v_type)
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
    // Postgres text-protocol for real[] arrives as '{a,b,c}'.
    try {
        const std::string s = rows[0]["vector"].as<std::string>();
        std::string body = s.substr(1, s.size() - 2);
        std::string tok;
        for (char c : body) {
            if (c == ',') {
                if (!tok.empty()) out_v_type.push_back(std::stof(tok));
                tok.clear();
            } else {
                tok.push_back(c);
            }
        }
        if (!tok.empty()) out_v_type.push_back(std::stof(tok));
    } catch (const std::exception& ex) {
        co_return std::unexpected(Error::database_error(std::format(
            "edge_type_vectors vector parse error: {}", ex.what())));
    }
    co_return Result<void>{};
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

// Split into two single-co_await helpers because GCC 14's coroutine
// frame destructor emitter ICEs (build_special_member_call at
// cp/call.cc:11096, inlining Error::~Error) on frames that contain
// two co_awaits together with any std::format-returning path — this
// combination pushed the destructor codegen past what the emitter
// handles. Keeping each helper to exactly one co_await is the smallest
// shape we've found that reliably survives.
drogon::Task<Result<void>>
EmbedEdgeWorker::fetch_endpoint_vectors(std::shared_ptr<rag::VectorStorePort> chunk_store,
                                        const std::string& ep0_pid,
                                        const std::string& ep1_pid,
                                        rag::Embedding&    out_v0,
                                        rag::Embedding&    out_v1)
{
    // Named lvalues only in and around the co_await: the CI compiler's
    // coroutine-frame emitter ICEs (build_special_member_call,
    // cp/call.cc:11096) on a braced-init temporary argument inside the
    // awaited call, and on resuming with a nested
    // expected<vector<pair<string, Embedding>>> — hence the hoisted ids
    // vector and the out-parameter port signature.
    std::vector<std::string> pids;
    pids.reserve(2);
    pids.push_back(ep0_pid);
    pids.push_back(ep1_pid);
    std::vector<std::pair<std::string, rag::Embedding>> vecs;
    {
        auto r = co_await chunk_store->fetch_vectors_by_id(pids, vecs);
        if (!r) co_return std::unexpected(r.error());
    }
    for (auto& kv : vecs) {
        if (kv.first == ep0_pid) out_v0 = std::move(kv.second);
        else if (kv.first == ep1_pid) out_v1 = std::move(kv.second);
    }
    co_return Result<void>{};
}

drogon::Task<Result<void>>
EmbedEdgeWorker::load_endpoint_vecs(const ClaimedEvent& ev,
                                    const LiveEdge&     live,
                                    std::shared_ptr<rag::VectorStorePort> chunk_store,
                                    rag::Embedding&     out_v0,
                                    rag::Embedding&     out_v1)
{
    std::string ep0_pid, ep1_pid;
    {
        auto r = co_await load_endpoint_point_ids(ev, live, ep0_pid, ep1_pid);
        if (!r) co_return std::unexpected(r.error());
    }
    {
        auto r = co_await fetch_endpoint_vectors(chunk_store, ep0_pid, ep1_pid,
                                                 out_v0, out_v1);
        if (!r) co_return std::unexpected(r.error());
    }
    if (out_v0.empty() || out_v1.empty()) {
        std::string msg = std::format(
            "Qdrant did not return one of the endpoint vectors "
            "(ep0='{}' ep1='{}'); retry", ep0_pid, ep1_pid);
        co_return std::unexpected(Error::unavailable(std::move(msg)));
    }
    co_return Result<void>{};
}

drogon::Task<Result<void>>
EmbedEdgeWorker::prepare_edge_vector(const ClaimedEvent& ev,
                                     const LiveEdge&     live,
                                     std::shared_ptr<rag::VectorStorePort> chunk_store,
                                     rag::Embedding&     out_edge_vec,
                                     std::string&        out_point_id)
{
    // Heap-allocate the Embedding locals so they do not live in this
    // coroutine's frame. GCC 14's coroutine-frame destructor emitter
    // ICEs when three std::vector<float> instances sit in the frame
    // together (build_special_member_call:cp/call.cc:11096); moving
    // them to the heap through a unique_ptr sidesteps that.
    struct Locals {
        rag::Embedding v0;
        rag::Embedding v1;
        rag::Embedding v_type;
    };
    auto s = std::make_unique<Locals>();

    // 4/4b. Endpoint vectors (own coroutine so the two round-trips
    // don't inflate this frame).
    {
        auto r = co_await load_endpoint_vecs(ev, live, chunk_store, s->v0, s->v1);
        if (!r) co_return std::unexpected(r.error());
    }

    // 5. Type vector.
    {
        auto r = co_await load_type_vector(ev, s->v_type);
        if (!r) co_return std::unexpected(r.error());
    }

    // 6. Formula v1 (pure).
    {
        auto r = rag::compute_edge_vector_v1(live.auth0, live.auth1, s->v0, s->v1, s->v_type);
        if (!r) co_return std::unexpected(r.error());
        out_edge_vec = std::move(*r);
    }

    // 7. Version-free point id (pure).
    out_point_id = compute_point_id(ev.edge_id,
                                    ev.embedding_model_id,
                                    ev.formula_version);
    co_return Result<void>{};
}

drogon::Task<Result<void>>
EmbedEdgeWorker::apply_edge_vector(const ClaimedEvent& ev,
                                   const LiveEdge&     live,
                                   const rag::Embedding& edge_vec,
                                   const std::string&    point_id)
{
    // 8. Qdrant upsert. LIVE payload fields, not event snapshot.
    {
        std::string payload_json = std::format(
            R"({{"company_id":"{}","edge_id":"{}","edge_type":"{}",)"
            R"("formula_version":{},"edge_version":{},)"
            R"("endpoint_0_chunk_id":"{}","endpoint_1_chunk_id":"{}",)"
            R"("authority_0":{},"authority_1":{},"review_state":"{}"}})",
            ev.company_id, ev.edge_id, live.edge_type,
            ev.formula_version, live.edge_version,
            live.ep0_chunk_id, live.ep1_chunk_id,
            live.auth0, live.auth1, live.review_state);
        auto r = co_await edge_store_->upsert_raw(point_id, edge_vec, std::move(payload_json));
        if (!r) co_return std::unexpected(r.error());
    }

    // 9. Bookkeeping.
    {
        auto r = co_await upsert_bookkeeping(ev, point_id, live.edge_version);
        if (!r) co_return std::unexpected(r.error());
    }
    co_return Result<void>{};
}

drogon::Task<Result<EmbedEdgeWorker::Outcome>>
EmbedEdgeWorker::write_edge_vector(const ClaimedEvent& ev,
                                   const LiveEdge&     live,
                                   const std::string&  collection,
                                   std::shared_ptr<rag::VectorStorePort> chunk_store)
{
    (void)collection;
    rag::Embedding edge_vec;
    std::string    pid;
    {
        auto r = co_await prepare_edge_vector(ev, live, chunk_store, edge_vec, pid);
        if (!r) co_return std::unexpected(r.error());
    }
    {
        auto r = co_await apply_edge_vector(ev, live, edge_vec, pid);
        if (!r) co_return std::unexpected(r.error());
    }
    co_return Outcome::Completed;
}

// ---------------------------------------------------------------------------
// Process one claimed event. Preflight only — hands off to
// write_edge_vector() for the terminal phase so each frame stays small.
// ---------------------------------------------------------------------------

drogon::Task<Result<EmbedEdgeWorker::Outcome>>
EmbedEdgeWorker::process(const ClaimedEvent& ev)
{
    // 1. Live-edge probe.
    LiveEdge live;
    {
        auto r = co_await load_live_edge(ev, live);
        if (!r) co_return std::unexpected(r.error());
        if (live.missing) co_return Outcome::NoEdge;
    }

    // 2. Staleness gate.
    {
        auto iev = co_await load_indexed_edge_version(ev);
        if (!iev) co_return std::unexpected(iev.error());
        if (*iev >= live.edge_version) co_return Outcome::Superseded;
    }

    // 3. Model collection.
    std::string collection;
    {
        auto r = co_await load_model_collection(ev.embedding_model_id, collection);
        if (!r) co_return std::unexpected(r.error());
        if (collection.empty()) co_return Outcome::ModelDisabled;
    }
    auto chunk_store = store_for_collection_(collection);
    if (!chunk_store)
        co_return std::unexpected(Error::invalid_state(std::format(
            "no vector store bound to collection '{}' — route a worker for it",
            collection)));

    // 4..9 in write_edge_vector().
    co_return co_await write_edge_vector(ev, live, collection, chunk_store);
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
