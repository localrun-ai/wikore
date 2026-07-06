#include <catch2/catch_test_macros.hpp>
#include "handlers.hpp"                          // wikore::api::wiki_evidence
#include "wikore/auth.hpp"                       // Identity
#include "wikore/rag/embedder.hpp"               // NullEmbedder
#include "wikore/rag/vector_store.hpp"           // NullVectorStore
#include "wikore/rag/relationship_vector_store.hpp" // NullRelationshipVectorStore
#include "wikore/rag/evidence_gate.hpp"
#include "wikore/rag/relationship_evidence_gate.hpp"
#include "wikore/access_resolver.hpp"
#include "wikore/db.hpp"

#include <drogon/drogon.h>
#include <drogon/utils/coroutine.h>
#include <glaze/glaze.hpp>

#include <chrono>
#include <cstdlib>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Integration test for the POST /api/orgs/{orgUnitId}/wiki/evidence handler
// (BaryGraph Lite step 8b). Exercises the intent field, the mixed-evidence
// response shape (each item carries a "kind" discriminator), and the fail-
// closed vs. degrade semantics inherited from retrieve_evidence.
//
// Uses NullEmbedder + NullVectorStore + NullRelationshipVectorStore against
// the REAL Postgres-backed gates so the auth + tenant + scope + G1 + G2
// pipeline runs against a live DB. Skips without DATABASE_URL.
// ---------------------------------------------------------------------------

namespace {

constexpr auto CO  = "05c00000-0000-0000-0000-0000000000e1"; // "evidence co"
constexpr auto CO2 = "05c00000-0000-0000-0000-0000000000e2"; // "evidence co B"
constexpr int  DIM = 4;

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

// Build an authenticated POST request with the "identity" attribute
// AuthFilter would have deposited.
drogon::HttpRequestPtr make_req(const std::string& user_id, const std::string& body)
{
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Post);
    req->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    req->setBody(body);
    wikore::Identity id;
    id.user_id      = user_id;
    id.email        = "member@test";
    id.display_name = "Member";
    req->getAttributes()->insert("identity", id);
    return req;
}

struct Fixture {
    std::string root;
    std::string team;
    std::string user;
    std::string foreign_root;
    std::string chunk_a;
    std::string chunk_b;
    std::string ver;
    std::shared_ptr<wikore::rag::NullVectorStore>              chunk_store;
    std::shared_ptr<wikore::rag::NullRelationshipVectorStore>  edge_store;
    std::optional<std::string> edge_id;
};

Fixture seed(drogon::orm::DbClientPtr db)
{
    exec_sync(db, "DELETE FROM companies WHERE id=$1::uuid", std::string(CO));
    exec_sync(db, "DELETE FROM companies WHERE id=$1::uuid", std::string(CO2));
    exec_sync(db, "INSERT INTO companies (id,name,slug) VALUES ($1::uuid,'EvA','ev-a')",
              std::string(CO));
    exec_sync(db, "INSERT INTO companies (id,name,slug) VALUES ($1::uuid,'EvB','ev-b')",
              std::string(CO2));

    Fixture f;
    f.root = sync_scalar(db, std::format(
        "SELECT id FROM org_units WHERE company_id='{}' AND type='root'", CO));
    f.team = sync_scalar(db, std::format(
        "INSERT INTO org_units (company_id,parent_id,type,slug,name) "
        "VALUES ('{}','{}','team','ev-team','Ev Team') RETURNING id", CO, f.root));
    f.foreign_root = sync_scalar(db, std::format(
        "SELECT id FROM org_units WHERE company_id='{}' AND type='root'", CO2));

    f.user = sync_scalar(db, std::format(
        "INSERT INTO users (company_id,external_sub,email) "
        "VALUES ('{}','sub-ev','ev@test') RETURNING id", CO));
    exec_sync(db, std::format(
        "INSERT INTO memberships (company_id,user_id,org_unit_id,role,applies_to) "
        "VALUES ('{}','{}','{}','viewer','self_and_descendants')",
        CO, f.user, f.team));

    const auto doc = sync_scalar(db, std::format(
        "INSERT INTO documents (company_id,owner_org_unit_id,filename,title,mime_type) "
        "VALUES ('{}','{}','ev.txt','Ev','text/plain') RETURNING id", CO, f.team));
    f.ver = sync_scalar(db, std::format(
        "INSERT INTO document_versions (company_id,document_id,version_no,source_hash,"
        "ingest_status,completed_at,activated_at,chunk_count,lifecycle_status,sensitivity_label) "
        "VALUES ('{}','{}',1,'h','done',now(),now(),2,'active','internal') RETURNING id",
        CO, doc));
    f.chunk_a = sync_scalar(db, std::format(
        "INSERT INTO document_chunks (company_id,document_version_id,chunk_index,"
        "content,content_hash,qdrant_prefilter_scope_ids) "
        "VALUES ('{}','{}',0,'chunk-A-text','h-a','{{}}') RETURNING id", CO, f.ver));
    f.chunk_b = sync_scalar(db, std::format(
        "INSERT INTO document_chunks (company_id,document_version_id,chunk_index,"
        "content,content_hash,qdrant_prefilter_scope_ids) "
        "VALUES ('{}','{}',1,'chunk-B-text','h-b','{{}}') RETURNING id", CO, f.ver));

    // Mirror both chunks in the Null chunk store.
    auto make_pt = [&](const std::string& chunk_id) {
        wikore::rag::ChunkPayload p;
        p.company_id          = CO;
        p.document_version_id = f.ver;
        p.chunk_id            = chunk_id;
        p.access_scope_ids    = {f.team};
        p.sensitivity_label   = "internal";
        p.lifecycle_status    = "active";
        return wikore::rag::UpsertPoint{
            .id = "pt-" + chunk_id,
            .vector = wikore::rag::Embedding(DIM, 0.1f),
            .payload = std::move(p)};
    };
    f.chunk_store = std::make_shared<wikore::rag::NullVectorStore>();
    drogon::sync_wait(f.chunk_store->upsert(std::vector<wikore::rag::UpsertPoint>{
        make_pt(f.chunk_a), make_pt(f.chunk_b),
    }));

    // Seed an edge and mirror it in the Null edge store so Bridge intent
    // can find it.
    auto edge_id = sync_scalar(db, "SELECT gen_random_uuid()");
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
        edge_id, CO, f.chunk_a, f.chunk_b));
    f.edge_id = edge_id;

    f.edge_store = std::make_shared<wikore::rag::NullRelationshipVectorStore>();
    f.edge_store->add(wikore::rag::EdgeCandidate{
        .edge_id             = edge_id,
        .company_id          = CO,
        .edge_type           = "implements",
        .score               = 0.95f,
        .edge_version        = 1,
        .formula_version     = 1,
        .endpoint_0_chunk_id = "",
        .endpoint_1_chunk_id = "",
    }, /*confidence=*/0.9, "accepted");
    return f;
}

std::shared_ptr<wikore::rag::RetrievalOrchestrator>
make_orch(drogon::orm::DbClientPtr                              db,
          std::shared_ptr<wikore::rag::VectorStorePort>         chunk_store,
          std::shared_ptr<wikore::rag::RelationshipVectorStorePort> edge_store)
{
    return std::make_shared<wikore::rag::RetrievalOrchestrator>(
        std::make_shared<wikore::rag::NullEmbedder>(DIM),
        std::make_shared<wikore::PostgresAccessResolver>(db),
        std::move(chunk_store),
        wikore::rag::EvidenceGate(db),
        std::move(edge_store),
        std::make_shared<wikore::rag::RelationshipEvidenceGate>(db));
}

// Minimal wire DTOs for parsing the response.
struct ItemDto {
    std::string kind;
    std::optional<std::string> chunk_id;
    std::optional<std::string> edge_id;
    std::optional<std::string> edge_type;
    std::optional<std::string> origin;
};
struct RespDto { std::vector<ItemDto> results; };

} // namespace

TEST_CASE("wiki_evidence: fact intent returns chunk-only kind",
          "[integration][api][evidence]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    auto f  = seed(db);
    auto orch = make_orch(db, f.chunk_store, f.edge_store);

    auto req  = make_req(f.user, R"({"query":"x","intent":"fact","limit":10})");
    auto resp = drogon::sync_wait(wikore::api::wiki_evidence(orch, db, req, f.team));

    REQUIRE(resp->getStatusCode() == drogon::k200OK);
    RespDto parsed;
    REQUIRE_FALSE(glz::read<glz::opts{.error_on_unknown_keys = false}>(
        parsed, std::string(resp->getBody())));
    REQUIRE(parsed.results.size() == 2);
    for (const auto& it : parsed.results) {
        CHECK(it.kind == "chunk");
        CHECK(it.chunk_id.has_value());
    }
}

TEST_CASE("wiki_evidence: bridge intent returns relationship-only kind with origin",
          "[integration][api][evidence]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    auto f  = seed(db);
    auto orch = make_orch(db, f.chunk_store, f.edge_store);

    auto req  = make_req(f.user, R"({"query":"x","intent":"bridge"})");
    auto resp = drogon::sync_wait(wikore::api::wiki_evidence(orch, db, req, f.team));

    REQUIRE(resp->getStatusCode() == drogon::k200OK);
    RespDto parsed;
    REQUIRE_FALSE(glz::read<glz::opts{.error_on_unknown_keys = false}>(
        parsed, std::string(resp->getBody())));
    REQUIRE(parsed.results.size() == 1);
    CHECK(parsed.results[0].kind == "relationship");
    CHECK(parsed.results[0].edge_id == f.edge_id);
    CHECK(parsed.results[0].edge_type == "implements");
    // origin surfaced from the V034 wire value (P3 vocabulary guard).
    CHECK(parsed.results[0].origin == "administrator");
}

TEST_CASE("wiki_evidence: automatic intent interleaves chunk and relationship kinds",
          "[integration][api][evidence]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    auto f  = seed(db);
    auto orch = make_orch(db, f.chunk_store, f.edge_store);

    auto req  = make_req(f.user, R"({"query":"x","intent":"automatic","limit":10})");
    auto resp = drogon::sync_wait(wikore::api::wiki_evidence(orch, db, req, f.team));

    REQUIRE(resp->getStatusCode() == drogon::k200OK);
    RespDto parsed;
    REQUIRE_FALSE(glz::read<glz::opts{.error_on_unknown_keys = false}>(
        parsed, std::string(resp->getBody())));
    // 2 chunks + 1 edge = 3 items, interleaved (chunk, edge, chunk).
    REQUIRE(parsed.results.size() == 3);
    CHECK(parsed.results[0].kind == "chunk");
    CHECK(parsed.results[1].kind == "relationship");
    CHECK(parsed.results[2].kind == "chunk");
}

TEST_CASE("wiki_evidence: bridge intent without edge collection fails closed (503)",
          "[integration][api][evidence]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    auto f  = seed(db);
    // Orchestrator WITHOUT edge_store + edge_gate: Bridge must fail closed.
    auto orch = std::make_shared<wikore::rag::RetrievalOrchestrator>(
        std::make_shared<wikore::rag::NullEmbedder>(DIM),
        std::make_shared<wikore::PostgresAccessResolver>(db),
        f.chunk_store,
        wikore::rag::EvidenceGate(db));

    auto req  = make_req(f.user, R"({"query":"x","intent":"bridge"})");
    auto resp = drogon::sync_wait(wikore::api::wiki_evidence(orch, db, req, f.team));
    CHECK(resp->getStatusCode() == drogon::k503ServiceUnavailable);
}

TEST_CASE("wiki_evidence: automatic intent degrades to fact-only when edges unwired",
          "[integration][api][evidence]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    auto f  = seed(db);
    auto orch = std::make_shared<wikore::rag::RetrievalOrchestrator>(
        std::make_shared<wikore::rag::NullEmbedder>(DIM),
        std::make_shared<wikore::PostgresAccessResolver>(db),
        f.chunk_store,
        wikore::rag::EvidenceGate(db));   // no edge store/gate

    auto req  = make_req(f.user, R"({"query":"x","intent":"automatic"})");
    auto resp = drogon::sync_wait(wikore::api::wiki_evidence(orch, db, req, f.team));

    REQUIRE(resp->getStatusCode() == drogon::k200OK);
    RespDto parsed;
    REQUIRE_FALSE(glz::read<glz::opts{.error_on_unknown_keys = false}>(
        parsed, std::string(resp->getBody())));
    // Chunk-only survivors.
    REQUIRE(parsed.results.size() == 2);
    for (const auto& it : parsed.results) CHECK(it.kind == "chunk");
}

TEST_CASE("wiki_evidence: absent intent field defaults to fact",
          "[integration][api][evidence]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    auto f  = seed(db);
    auto orch = make_orch(db, f.chunk_store, f.edge_store);

    auto req  = make_req(f.user, R"({"query":"x"})");   // no intent
    auto resp = drogon::sync_wait(wikore::api::wiki_evidence(orch, db, req, f.team));

    REQUIRE(resp->getStatusCode() == drogon::k200OK);
    RespDto parsed;
    REQUIRE_FALSE(glz::read<glz::opts{.error_on_unknown_keys = false}>(
        parsed, std::string(resp->getBody())));
    REQUIRE(parsed.results.size() == 2);
    for (const auto& it : parsed.results) CHECK(it.kind == "chunk");
}

TEST_CASE("wiki_evidence: input validation (400 / 401 / 404)",
          "[integration][api][evidence]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    auto f  = seed(db);
    auto orch = make_orch(db, f.chunk_store, f.edge_store);

    SECTION("unknown intent -> 400") {
        auto resp = drogon::sync_wait(wikore::api::wiki_evidence(
            orch, db, make_req(f.user, R"({"query":"x","intent":"nonsense"})"),
            f.team));
        CHECK(resp->getStatusCode() == drogon::k400BadRequest);
    }
    SECTION("empty query -> 400") {
        auto resp = drogon::sync_wait(wikore::api::wiki_evidence(
            orch, db, make_req(f.user, R"({"query":"   ","intent":"fact"})"),
            f.team));
        CHECK(resp->getStatusCode() == drogon::k400BadRequest);
    }
    SECTION("non-positive limit -> 400") {
        auto resp = drogon::sync_wait(wikore::api::wiki_evidence(
            orch, db, make_req(f.user, R"({"query":"x","limit":0})"),
            f.team));
        CHECK(resp->getStatusCode() == drogon::k400BadRequest);
    }
    SECTION("empty allowed_review_states -> 400") {
        auto resp = drogon::sync_wait(wikore::api::wiki_evidence(
            orch, db, make_req(f.user,
                R"({"query":"x","intent":"bridge","allowed_review_states":[]})"),
            f.team));
        CHECK(resp->getStatusCode() == drogon::k400BadRequest);
    }
    SECTION("out-of-range min_confidence -> 400") {
        auto resp = drogon::sync_wait(wikore::api::wiki_evidence(
            orch, db, make_req(f.user,
                R"({"query":"x","intent":"bridge","min_confidence":1.5})"),
            f.team));
        CHECK(resp->getStatusCode() == drogon::k400BadRequest);
    }
    SECTION("malformed JSON -> 400") {
        auto resp = drogon::sync_wait(wikore::api::wiki_evidence(
            orch, db, make_req(f.user, "{not json"), f.team));
        CHECK(resp->getStatusCode() == drogon::k400BadRequest);
    }
    SECTION("non-uuid org unit -> 404") {
        auto resp = drogon::sync_wait(wikore::api::wiki_evidence(
            orch, db, make_req(f.user, R"({"query":"x"})"), "not-a-uuid"));
        CHECK(resp->getStatusCode() == drogon::k404NotFound);
    }
    SECTION("cross-tenant org unit -> 404 (no existence leak)") {
        auto resp = drogon::sync_wait(wikore::api::wiki_evidence(
            orch, db, make_req(f.user, R"({"query":"x"})"), f.foreign_root));
        CHECK(resp->getStatusCode() == drogon::k404NotFound);
    }
    SECTION("missing identity -> 401") {
        auto req = drogon::HttpRequest::newHttpRequest();
        req->setMethod(drogon::Post);
        req->setBody(R"({"query":"x"})");
        auto resp = drogon::sync_wait(wikore::api::wiki_evidence(
            orch, db, req, f.team));
        CHECK(resp->getStatusCode() == drogon::k401Unauthorized);
    }
}

TEST_CASE("wiki_evidence: deactivated user cannot retrieve (403)",
          "[integration][api][evidence]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    auto f  = seed(db);
    auto orch = make_orch(db, f.chunk_store, f.edge_store);

    exec_sync(db, std::format(
        "UPDATE users SET deactivated_at = now() WHERE id = '{}'", f.user));

    auto resp = drogon::sync_wait(wikore::api::wiki_evidence(
        orch, db, make_req(f.user, R"({"query":"x"})"), f.team));
    CHECK(resp->getStatusCode() == drogon::k403Forbidden);
}
