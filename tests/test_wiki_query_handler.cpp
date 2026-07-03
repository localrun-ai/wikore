#include <catch2/catch_test_macros.hpp>
#include "handlers.hpp"                  // wikore::api::wiki_query
#include "wikore/auth.hpp"              // Identity
#include "wikore/rag/embedder.hpp"      // NullEmbedder
#include "wikore/rag/vector_store.hpp"  // NullVectorStore
#include "wikore/rag/evidence_gate.hpp"
#include "wikore/access_resolver.hpp"
#include "wikore/db.hpp"

#include <drogon/drogon.h>
#include <drogon/utils/coroutine.h>
#include <glaze/glaze.hpp>

#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

// Integration test for the POST /api/orgs/{orgUnitId}/wiki/query handler.
// Drives wikore::api::wiki_query directly (the executable is not linkable from
// tests) with a NullEmbedder + in-memory NullVectorStore and the REAL resolver
// and gate against Postgres, exercising tenant-from-auth resolution, scope
// validation, body validation, and the 200 serialization shape. Skips without
// DATABASE_URL.

namespace {

constexpr auto CO  = "05c00000-0000-0000-0000-0000000000b1"; // tenant A
constexpr auto CO2 = "05c00000-0000-0000-0000-0000000000b2"; // tenant B (foreign)
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

std::string root_of(drogon::orm::DbClientPtr db, const char* co)
{
    return std::string(exec_sync(db,
        "SELECT id FROM org_units WHERE company_id=$1::uuid AND type='root'",
        std::string(co))[0]["id"].c_str());
}

// Build an authenticated POST request: JSON body + the "identity" attribute
// that AuthFilter would have deposited on a live request.
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

std::shared_ptr<wikore::rag::RetrievalOrchestrator>
make_orch(drogon::orm::DbClientPtr db, std::shared_ptr<wikore::rag::VectorStorePort> store)
{
    return std::make_shared<wikore::rag::RetrievalOrchestrator>(
        std::make_shared<wikore::rag::NullEmbedder>(DIM),
        std::make_shared<wikore::PostgresAccessResolver>(db),
        std::move(store),
        wikore::rag::EvidenceGate(db));
}

struct Fixture {
    std::string root_a;      // in-scope org unit (tenant A root)
    std::string foreign;     // org unit belonging to tenant B
    std::string user;        // member of tenant A
    std::string chunk_id;    // the single visible chunk
    std::shared_ptr<wikore::rag::NullVectorStore> store;
};

Fixture seed(drogon::orm::DbClientPtr db)
{
    exec_sync(db, "DELETE FROM companies WHERE id=$1::uuid", std::string(CO));
    exec_sync(db, "DELETE FROM companies WHERE id=$1::uuid", std::string(CO2));
    exec_sync(db, "INSERT INTO companies (id,name,slug) VALUES ($1::uuid,'WqA','wq-a')",
              std::string(CO));
    exec_sync(db, "INSERT INTO companies (id,name,slug) VALUES ($1::uuid,'WqB','wq-b')",
              std::string(CO2));

    Fixture f;
    f.root_a  = root_of(db, CO);
    f.foreign = root_of(db, CO2);

    // Reader is a viewer of the tenant-A root (and descendants).
    f.user = std::string(exec_sync(db,
        "INSERT INTO users (company_id,external_sub,email) "
        "VALUES ($1::uuid,'subWq','wq@test') RETURNING id", std::string(CO))[0]["id"].c_str());
    exec_sync(db,
        "INSERT INTO memberships (company_id,user_id,org_unit_id,role,applies_to) "
        "VALUES ($1::uuid,$2::uuid,$3::uuid,'viewer','self_and_descendants')",
        std::string(CO), f.user, f.root_a);

    // One active, internal document owned by the root org unit.
    const auto doc = std::string(exec_sync(db,
        "INSERT INTO documents (company_id,owner_org_unit_id,filename) "
        "VALUES ($1::uuid,$2::uuid,'wq.txt') RETURNING id",
        std::string(CO), f.root_a)[0]["id"].c_str());
    const auto ver = std::string(exec_sync(db,
        "INSERT INTO document_versions (company_id,document_id,version_no,source_hash,"
        "ingest_status,completed_at,activated_at,chunk_count,lifecycle_status,sensitivity_label) "
        "VALUES ($1::uuid,$2::uuid,1,'h','done',now(),now(),1,'active','internal') RETURNING id",
        std::string(CO), doc)[0]["id"].c_str());
    f.chunk_id = std::string(exec_sync(db,
        "INSERT INTO document_chunks (company_id,document_version_id,chunk_index,content,"
        "content_hash,qdrant_prefilter_scope_ids) VALUES ($1::uuid,$2::uuid,0,'the leave policy body',"
        "'hwq','{}') RETURNING id", std::string(CO), ver)[0]["id"].c_str());

    // Mirror the chunk as a Qdrant point with a matching payload scope.
    wikore::rag::ChunkPayload p;
    p.company_id          = CO;
    p.document_version_id = ver;
    p.chunk_id            = f.chunk_id;
    p.access_scope_ids    = {f.root_a};
    p.sensitivity_label   = "internal";
    p.lifecycle_status    = "active";
    f.store = std::make_shared<wikore::rag::NullVectorStore>();
    drogon::sync_wait(f.store->upsert(std::vector<wikore::rag::UpsertPoint>{
        {.id = "pt-" + f.chunk_id, .vector = wikore::rag::Embedding(DIM, 0.1f),
         .payload = std::move(p)}}));
    return f;
}

struct ResultRow  { std::string chunk_id; };
struct ParsedResp { std::vector<ResultRow> results; };

int count_results(const std::string& body)
{
    ParsedResp r;
    if (glz::read<glz::opts{.error_on_unknown_keys = false}>(r, body)) return -1;
    return static_cast<int>(r.results.size());
}

} // namespace

TEST_CASE("wiki_query: in-scope query returns gated results (200)",
          "[integration][api]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    auto f  = seed(db);
    auto orch = make_orch(db, f.store);

    auto req  = make_req(f.user, R"({"query":"leave policy","limit":5})");
    auto resp = drogon::sync_wait(wikore::api::wiki_query(orch, db, req, f.root_a));

    REQUIRE(resp->getStatusCode() == drogon::k200OK);
    CHECK(count_results(std::string(resp->getBody())) == 1);
    CHECK(std::string(resp->getBody()).find(f.chunk_id) != std::string::npos);
    CHECK(std::string(resp->getBody()).find("leave policy body") != std::string::npos);
}

TEST_CASE("wiki_query: org unit from another tenant is 404 (no cross-tenant leak)",
          "[integration][api]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    auto f  = seed(db);
    auto orch = make_orch(db, f.store);

    // f.foreign is a real org unit, but it belongs to tenant B; the caller is a
    // tenant-A user. Existence must not be distinguishable from absence.
    auto req  = make_req(f.user, R"({"query":"leave policy"})");
    auto resp = drogon::sync_wait(wikore::api::wiki_query(orch, db, req, f.foreign));

    CHECK(resp->getStatusCode() == drogon::k404NotFound);
}

TEST_CASE("wiki_query: input validation (400 / 404)", "[integration][api]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    auto f  = seed(db);
    auto orch = make_orch(db, f.store);

    SECTION("empty query -> 400") {
        auto resp = drogon::sync_wait(wikore::api::wiki_query(
            orch, db, make_req(f.user, R"({"query":"   "})"), f.root_a));
        CHECK(resp->getStatusCode() == drogon::k400BadRequest);
    }
    SECTION("non-positive limit -> 400") {
        auto resp = drogon::sync_wait(wikore::api::wiki_query(
            orch, db, make_req(f.user, R"({"query":"x","limit":0})"), f.root_a));
        CHECK(resp->getStatusCode() == drogon::k400BadRequest);
    }
    SECTION("malformed JSON -> 400") {
        auto resp = drogon::sync_wait(wikore::api::wiki_query(
            orch, db, make_req(f.user, "{not json"), f.root_a));
        CHECK(resp->getStatusCode() == drogon::k400BadRequest);
    }
    SECTION("non-uuid org unit -> 404") {
        auto resp = drogon::sync_wait(wikore::api::wiki_query(
            orch, db, make_req(f.user, R"({"query":"x"})"), "not-a-uuid"));
        CHECK(resp->getStatusCode() == drogon::k404NotFound);
    }
}

TEST_CASE("wiki_query: missing identity attribute is 401", "[integration][api]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    auto f  = seed(db);
    auto orch = make_orch(db, f.store);

    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Post);
    req->setBody(R"({"query":"x"})");  // no "identity" attribute
    auto resp = drogon::sync_wait(wikore::api::wiki_query(orch, db, req, f.root_a));

    CHECK(resp->getStatusCode() == drogon::k401Unauthorized);
}
