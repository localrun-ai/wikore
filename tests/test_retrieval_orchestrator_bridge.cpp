#include <catch2/catch_test_macros.hpp>
#include "wikore/rag/retrieval_orchestrator.hpp"
#include "wikore/rag/embedder.hpp"       // NullEmbedder
#include "wikore/rag/vector_store.hpp"   // NullVectorStore
#include "wikore/rag/relationship_vector_store.hpp"   // NullRelationshipVectorStore
#include "wikore/adapters/postgres/deadline_exec.hpp"
#include "wikore/access_resolver.hpp"
#include "wikore/db.hpp"
#include <drogon/drogon.h>
#include <drogon/utils/coroutine.h>
#include <chrono>
#include <cstdlib>
#include <format>
#include <memory>
#include <string>
#include <variant>
#include <vector>

// ---------------------------------------------------------------------------
// Integration test suite for RetrievalOrchestrator::retrieve_evidence — the
// intent-dispatched entry point that covers Fact / Bridge / Automatic per
// docs §"Fact intent" / §"Bridge intent" / §"Automatic intent".
//
// Uses NullEmbedder + NullVectorStore + NullRelationshipVectorStore with a
// REAL Postgres-backed AccessResolver, EvidenceGate (G1) and
// RelationshipEvidenceGate (G2). Skip without DATABASE_URL — the gates are
// SQL, not stubs, so an in-memory PG substitute would silently pass a broken
// gate.
//
// The suite verifies:
//   1. Fact intent returns AllowedChunk-only variants;
//   2. Bridge intent returns AllowedRelationship-only variants;
//   3. Bridge intent fails closed (unavailable) when no edge store/gate;
//   4. Automatic intent returns interleaved (chunk, edge, ...);
//   5. Automatic gracefully degrades to fact-only when no edge store/gate;
//   6. Automatic reuses the same embedding for chunk + edge sub-searches
//      (asserted via the NullEmbedder call count);
//   7. Non-positive limit, empty review_state, and deadline exceeded fail
//      early on all intent paths.
// ---------------------------------------------------------------------------

namespace {

constexpr auto CO_ORCH = "16e20000-0000-0000-0000-000000000cb1";  // "orch bridge"
constexpr int  DIM     = 4;

bool db_available() { return std::getenv("DATABASE_URL") != nullptr; }

template<typename... Args>
drogon::orm::Result exec_sync(drogon::orm::DbClientPtr db, std::string sql, Args... args)
{
    return drogon::sync_wait(
        [db, sql = std::move(sql), ...args = std::move(args)]()
        -> drogon::Task<drogon::orm::Result> {
            co_return co_await db->execSqlCoro(sql, args...);
        }());
}

std::string sync_scalar(drogon::orm::DbClientPtr db, std::string sql)
{
    auto rows = exec_sync(db, std::move(sql));
    return std::string(rows[0][0].c_str());
}

struct Seed {
    std::string root;
    std::string team;
    std::string user;
    std::string doc;
    std::string ver;
    std::string chunk_a;
    std::string chunk_b;
};

Seed seed(drogon::orm::DbClientPtr db)
{
    exec_sync(db, "DELETE FROM companies WHERE id=$1::uuid", std::string(CO_ORCH));
    exec_sync(db, "INSERT INTO companies (id,name,slug) VALUES ($1::uuid,'OrchB','orch-b')",
              std::string(CO_ORCH));

    Seed s;
    s.root = sync_scalar(db, std::format(
        "SELECT id FROM org_units WHERE company_id='{}' AND type='root'", CO_ORCH));
    s.team = sync_scalar(db, std::format(
        "INSERT INTO org_units (company_id,parent_id,type,slug,name) "
        "VALUES ('{}','{}','team','tt','Team T') RETURNING id", CO_ORCH, s.root));

    s.user = sync_scalar(db, std::format(
        "INSERT INTO users (company_id,external_sub,email) "
        "VALUES ('{}','sub-orch-b','orch-b@test') RETURNING id", CO_ORCH));
    exec_sync(db, std::format(
        "INSERT INTO memberships (company_id,user_id,org_unit_id,role,applies_to) "
        "VALUES ('{}','{}','{}','viewer','self_and_descendants')",
        CO_ORCH, s.user, s.team));

    s.doc = sync_scalar(db, std::format(
        "INSERT INTO documents (company_id,owner_org_unit_id,filename,title,mime_type) "
        "VALUES ('{}','{}','d.txt','D','text/plain') RETURNING id", CO_ORCH, s.team));
    s.ver = sync_scalar(db, std::format(
        "INSERT INTO document_versions (company_id,document_id,version_no,source_hash,"
        "ingest_status,completed_at,activated_at,chunk_count,lifecycle_status,sensitivity_label) "
        "VALUES ('{}','{}',1,'h','done',now(),now(),2,'active','internal') RETURNING id",
        CO_ORCH, s.doc));
    s.chunk_a = sync_scalar(db, std::format(
        "INSERT INTO document_chunks (company_id,document_version_id,chunk_index,"
        "content,content_hash,qdrant_prefilter_scope_ids) "
        "VALUES ('{}','{}',0,'chunk-A-text','h-a','{{}}') RETURNING id", CO_ORCH, s.ver));
    s.chunk_b = sync_scalar(db, std::format(
        "INSERT INTO document_chunks (company_id,document_version_id,chunk_index,"
        "content,content_hash,qdrant_prefilter_scope_ids) "
        "VALUES ('{}','{}',1,'chunk-B-text','h-b','{{}}') RETURNING id", CO_ORCH, s.ver));
    return s;
}

// Insert an accepted knowledge_edge between two chunks via the single-CTE
// pattern the repo uses (statement-level trigger fires at commit).
std::string make_edge(drogon::orm::DbClientPtr db,
                      const std::string& chunk0, const std::string& chunk1)
{
    auto id = sync_scalar(db, "SELECT gen_random_uuid()");
    exec_sync(db, std::format(
        "WITH new_edge AS ( "
        "  INSERT INTO knowledge_edges "
        "    (id, company_id, edge_type, direction, confidence, origin, review_state) "
        "  VALUES ('{}','{}','implements','directed',0.9,'administrator','accepted') "
        "  RETURNING id, company_id "
        ") "
        "INSERT INTO knowledge_edge_endpoints "
        "  (company_id, edge_id, ordinal, chunk_id, role) "
        "SELECT company_id, id, 0, '{}'::uuid, 'source' FROM new_edge "
        "UNION ALL "
        "SELECT company_id, id, 1, '{}'::uuid, 'target' FROM new_edge",
        id, CO_ORCH, chunk0, chunk1));
    return id;
}

wikore::rag::EdgeCandidate cand(const std::string& edge_id, float score = 0.9f)
{
    return wikore::rag::EdgeCandidate{
        .edge_id             = edge_id,
        .company_id          = CO_ORCH,
        .edge_type           = "implements",
        .score               = score,
        .edge_version        = 1,
        .formula_version     = 1,
        .endpoint_0_chunk_id = "",
        .endpoint_1_chunk_id = "",
    };
}

wikore::rag::UpsertPoint chunk_point(const std::string& chunk_id,
                                     const std::string& ver_id,
                                     const std::string& ou)
{
    wikore::rag::ChunkPayload p;
    p.company_id          = CO_ORCH;
    p.document_version_id = ver_id;
    p.chunk_id            = chunk_id;
    p.access_scope_ids    = {ou};
    p.sensitivity_label   = "internal";
    p.lifecycle_status    = "active";
    return {.id = "pt-" + chunk_id, .vector = wikore::rag::Embedding(DIM, 0.1f),
            .payload = std::move(p)};
}

// Instrumented embedder wrapping NullEmbedder — lets us assert the embedder
// is called EXACTLY ONCE per retrieve_evidence call, even for Automatic
// intent (which internally shares the embedding across both sub-searches).
class CountingEmbedder : public wikore::rag::EmbedderPort {
public:
    explicit CountingEmbedder(int dim) : _inner(dim) {}
    drogon::Task<wikore::Result<wikore::rag::Embedding>>
    embed(std::string text, double timeout_s = 0) override
    {
        ++_calls;
        co_return co_await _inner.embed(std::move(text), timeout_s);
    }
    drogon::Task<wikore::Result<std::vector<wikore::rag::Embedding>>>
    embed_batch(std::vector<std::string> texts) override
    {
        co_return co_await _inner.embed_batch(std::move(texts));
    }
    int dims() const override { return _inner.dims(); }
    const std::string& model_name() const override { return _inner.model_name(); }
    int calls() const { return _calls; }
private:
    wikore::rag::NullEmbedder _inner;
    int                       _calls = 0;
};

wikore::RequestContext make_ctx(const std::string& user_id)
{
    return wikore::RequestContext{
        .tenant    = {.company_id = CO_ORCH},
        .principal = {.user_id = user_id, .email = "orch-b@test"},
        .span      = {},
        .deadline  = std::chrono::steady_clock::now() + std::chrono::seconds(30),
    };
}

wikore::rag::BridgeIntentOptions default_bridge_opts()
{
    return wikore::rag::BridgeIntentOptions{
        .min_confidence = 0.0,   // NullRelationshipVectorStore ignores this
        .allowed_review_states = {"accepted"},
    };
}

} // namespace

TEST_CASE("retrieve_evidence Fact returns AllowedChunk-only variants",
          "[integration][orchestrator][bridge]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    auto s  = seed(db);

    auto chunks = std::make_shared<wikore::rag::NullVectorStore>();
    drogon::sync_wait(chunks->upsert(std::vector<wikore::rag::UpsertPoint>{
        chunk_point(s.chunk_a, s.ver, s.team),
        chunk_point(s.chunk_b, s.ver, s.team),
    }));

    auto embedder = std::make_shared<CountingEmbedder>(DIM);
    wikore::rag::RetrievalOrchestrator orch(
        embedder,
        std::make_shared<wikore::PostgresAccessResolver>(db),
        chunks,
        wikore::rag::EvidenceGate(db));   // no edge store/gate — Fact is unaffected

    auto ctx = make_ctx(s.user);
    auto r = drogon::sync_wait(orch.retrieve_evidence(
        ctx, "q", s.team, wikore::rag::RetrievalIntent::Fact,
        default_bridge_opts(), 10));
    REQUIRE(r.has_value());
    REQUIRE(r->size() == 2);
    for (const auto& e : *r)
        CHECK(std::holds_alternative<wikore::rag::AllowedChunk>(e));
    CHECK(embedder->calls() == 1);
}

TEST_CASE("retrieve_evidence Bridge returns AllowedRelationship-only variants",
          "[integration][orchestrator][bridge]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    auto s  = seed(db);

    auto edge_id = make_edge(db, s.chunk_a, s.chunk_b);

    auto edges = std::make_shared<wikore::rag::NullRelationshipVectorStore>();
    edges->add(cand(edge_id, 0.95f), /*confidence=*/0.9, "accepted");

    auto embedder = std::make_shared<CountingEmbedder>(DIM);
    wikore::rag::RetrievalOrchestrator orch(
        embedder,
        std::make_shared<wikore::PostgresAccessResolver>(db),
        std::make_shared<wikore::rag::NullVectorStore>(),
        wikore::rag::EvidenceGate(db),
        edges,
        std::make_shared<wikore::rag::RelationshipEvidenceGate>(db));

    auto ctx = make_ctx(s.user);
    auto r = drogon::sync_wait(orch.retrieve_evidence(
        ctx, "q", s.team, wikore::rag::RetrievalIntent::Bridge,
        default_bridge_opts(), 10));
    REQUIRE(r.has_value());
    REQUIRE(r->size() == 1);
    REQUIRE(std::holds_alternative<wikore::rag::AllowedRelationship>((*r)[0]));
    const auto& rel = std::get<wikore::rag::AllowedRelationship>((*r)[0]);
    CHECK(rel.edge_id() == edge_id);
    CHECK(rel.endpoint_0().text == "chunk-A-text");
    CHECK(rel.endpoint_1().text == "chunk-B-text");
    CHECK(embedder->calls() == 1);
}

TEST_CASE("retrieve_evidence Bridge fails closed when no edge store/gate configured",
          "[integration][orchestrator][bridge]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    auto s  = seed(db);

    wikore::rag::RetrievalOrchestrator orch(
        std::make_shared<wikore::rag::NullEmbedder>(DIM),
        std::make_shared<wikore::PostgresAccessResolver>(db),
        std::make_shared<wikore::rag::NullVectorStore>(),
        wikore::rag::EvidenceGate(db));   // edge_store = edge_gate = nullptr

    auto ctx = make_ctx(s.user);
    auto r = drogon::sync_wait(orch.retrieve_evidence(
        ctx, "q", s.team, wikore::rag::RetrievalIntent::Bridge,
        default_bridge_opts(), 10));
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().kind == wikore::Error::Kind::ServiceUnavailable);
}

TEST_CASE("retrieve_evidence Automatic interleaves chunk and edge results",
          "[integration][orchestrator][bridge]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    auto s  = seed(db);

    auto edge_id = make_edge(db, s.chunk_a, s.chunk_b);

    auto chunks = std::make_shared<wikore::rag::NullVectorStore>();
    drogon::sync_wait(chunks->upsert(std::vector<wikore::rag::UpsertPoint>{
        chunk_point(s.chunk_a, s.ver, s.team),
        chunk_point(s.chunk_b, s.ver, s.team),
    }));

    auto edges = std::make_shared<wikore::rag::NullRelationshipVectorStore>();
    edges->add(cand(edge_id, 0.95f), /*confidence=*/0.9, "accepted");

    auto embedder = std::make_shared<CountingEmbedder>(DIM);
    wikore::rag::RetrievalOrchestrator orch(
        embedder,
        std::make_shared<wikore::PostgresAccessResolver>(db),
        chunks,
        wikore::rag::EvidenceGate(db),
        edges,
        std::make_shared<wikore::rag::RelationshipEvidenceGate>(db));

    auto ctx = make_ctx(s.user);
    auto r = drogon::sync_wait(orch.retrieve_evidence(
        ctx, "q", s.team, wikore::rag::RetrievalIntent::Automatic,
        default_bridge_opts(), 10));
    REQUIRE(r.has_value());
    // 2 chunks + 1 edge = 3 interleaved: [chunk, edge, chunk].
    REQUIRE(r->size() == 3);
    CHECK(std::holds_alternative<wikore::rag::AllowedChunk>((*r)[0]));
    CHECK(std::holds_alternative<wikore::rag::AllowedRelationship>((*r)[1]));
    CHECK(std::holds_alternative<wikore::rag::AllowedChunk>((*r)[2]));

    // Automatic MUST share one embedding across both sub-searches — else
    // every automatic request pays a double embedder round-trip.
    CHECK(embedder->calls() == 1);
}

TEST_CASE("retrieve_evidence Automatic degrades to fact-only when no edge store/gate",
          "[integration][orchestrator][bridge]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    auto s  = seed(db);

    auto chunks = std::make_shared<wikore::rag::NullVectorStore>();
    drogon::sync_wait(chunks->upsert(std::vector<wikore::rag::UpsertPoint>{
        chunk_point(s.chunk_a, s.ver, s.team),
    }));

    wikore::rag::RetrievalOrchestrator orch(
        std::make_shared<wikore::rag::NullEmbedder>(DIM),
        std::make_shared<wikore::PostgresAccessResolver>(db),
        chunks,
        wikore::rag::EvidenceGate(db));   // no edge store/gate

    auto ctx = make_ctx(s.user);
    auto r = drogon::sync_wait(orch.retrieve_evidence(
        ctx, "q", s.team, wikore::rag::RetrievalIntent::Automatic,
        default_bridge_opts(), 10));
    REQUIRE(r.has_value());
    REQUIRE(r->size() == 1);
    CHECK(std::holds_alternative<wikore::rag::AllowedChunk>((*r)[0]));
}

TEST_CASE("retrieve_evidence rejects non-positive limit on every intent",
          "[integration][orchestrator][bridge]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    auto s  = seed(db);

    auto edges = std::make_shared<wikore::rag::NullRelationshipVectorStore>();
    wikore::rag::RetrievalOrchestrator orch(
        std::make_shared<wikore::rag::NullEmbedder>(DIM),
        std::make_shared<wikore::PostgresAccessResolver>(db),
        std::make_shared<wikore::rag::NullVectorStore>(),
        wikore::rag::EvidenceGate(db),
        edges,
        std::make_shared<wikore::rag::RelationshipEvidenceGate>(db));

    auto ctx = make_ctx(s.user);
    for (auto intent : {wikore::rag::RetrievalIntent::Fact,
                        wikore::rag::RetrievalIntent::Bridge,
                        wikore::rag::RetrievalIntent::Automatic}) {
        for (int bad : {0, -1}) {
            auto r = drogon::sync_wait(orch.retrieve_evidence(
                ctx, "q", s.team, intent, default_bridge_opts(), bad));
            REQUIRE_FALSE(r.has_value());
            CHECK(r.error().kind == wikore::Error::Kind::InvalidInput);
        }
    }
}

TEST_CASE("retrieve_evidence Bridge and Automatic reject empty allowed_review_states",
          "[integration][orchestrator][bridge]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    auto s  = seed(db);

    auto edges = std::make_shared<wikore::rag::NullRelationshipVectorStore>();
    wikore::rag::RetrievalOrchestrator orch(
        std::make_shared<wikore::rag::NullEmbedder>(DIM),
        std::make_shared<wikore::PostgresAccessResolver>(db),
        std::make_shared<wikore::rag::NullVectorStore>(),
        wikore::rag::EvidenceGate(db),
        edges,
        std::make_shared<wikore::rag::RelationshipEvidenceGate>(db));

    wikore::rag::BridgeIntentOptions bad{.min_confidence = 0.0,
                                         .allowed_review_states = {}};
    auto ctx = make_ctx(s.user);
    for (auto intent : {wikore::rag::RetrievalIntent::Bridge,
                        wikore::rag::RetrievalIntent::Automatic}) {
        auto r = drogon::sync_wait(orch.retrieve_evidence(
            ctx, "q", s.team, intent, bad, 10));
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().kind == wikore::Error::Kind::InvalidInput);
    }

    // Fact intent never hits the edge path — empty review states are
    // irrelevant, so the request must succeed.
    auto r = drogon::sync_wait(orch.retrieve_evidence(
        ctx, "q", s.team, wikore::rag::RetrievalIntent::Fact, bad, 10));
    CHECK(r.has_value());
}
