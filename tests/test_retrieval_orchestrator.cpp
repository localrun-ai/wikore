#include <catch2/catch_test_macros.hpp>
#include "wikore/rag/retrieval_orchestrator.hpp"
#include "wikore/rag/embedder.hpp"       // NullEmbedder
#include "wikore/rag/vector_store.hpp"   // NullVectorStore
#include "wikore/access_resolver.hpp"
#include "wikore/db.hpp"
#include <drogon/drogon.h>
#include <drogon/utils/coroutine.h>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

// Integration test for the retrieval orchestrator: embed -> resolve scope ->
// derive clearance -> prefilter -> vector search -> EvidenceGate. Uses a
// NullEmbedder + an in-memory NullVectorStore (seeded to mimic Qdrant) with the
// REAL resolver and gate against Postgres, so it exercises the whole read-path
// wiring end to end. Skip without DATABASE_URL.

namespace {

constexpr auto CO  = "05c00000-0000-0000-0000-0000000000a1"; // "orchestrator co"
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

std::string g_root;
void seed(drogon::orm::DbClientPtr db)
{
    exec_sync(db, "DELETE FROM companies WHERE id=$1::uuid", std::string(CO));
    exec_sync(db, "INSERT INTO companies (id,name,slug) VALUES ($1::uuid,'Orch','orch')",
              std::string(CO));
    g_root = std::string(exec_sync(db,
        "SELECT id FROM org_units WHERE company_id=$1::uuid AND type='root'",
        std::string(CO))[0]["id"].c_str());
}

std::string make_ou(drogon::orm::DbClientPtr db, const std::string& parent, const std::string& slug)
{
    return std::string(exec_sync(db,
        "INSERT INTO org_units (company_id,parent_id,type,slug,name) "
        "VALUES ($1::uuid,$2::uuid,'team',$3,$3) RETURNING id",
        std::string(CO), parent, slug)[0]["id"].c_str());
}

struct ChunkRef { std::string chunk_id, version_id; };
ChunkRef make_chunk(drogon::orm::DbClientPtr db, const std::string& owner,
                    const std::string& sensitivity)
{
    static int n = 0; ++n;
    const auto doc = std::string(exec_sync(db,
        "INSERT INTO documents (company_id,owner_org_unit_id,filename) "
        "VALUES ($1::uuid,$2::uuid,$3) RETURNING id",
        std::string(CO), owner, "d" + std::to_string(n) + ".txt")[0]["id"].c_str());
    const auto ver = std::string(exec_sync(db,
        "INSERT INTO document_versions (company_id,document_id,version_no,source_hash,"
        "ingest_status,completed_at,activated_at,chunk_count,lifecycle_status,sensitivity_label) "
        "VALUES ($1::uuid,$2::uuid,1,'h','done',now(),now(),1,'active',$3) RETURNING id",
        std::string(CO), doc, sensitivity)[0]["id"].c_str());
    const auto chunk = std::string(exec_sync(db,
        "INSERT INTO document_chunks (company_id,document_version_id,chunk_index,content,"
        "content_hash,qdrant_prefilter_scope_ids) VALUES ($1::uuid,$2::uuid,0,$3,$4,'{}') "
        "RETURNING id", std::string(CO), ver, "body " + doc, "h" + doc)[0]["id"].c_str());
    return {chunk, ver};
}

// A Qdrant-style point mirroring a chunk. payload_scope is the (possibly stale)
// access_scope_ids encoded in the payload; sensitivity is the payload label.
wikore::rag::UpsertPoint point_for(const ChunkRef& r,
                                   const std::vector<std::string>& payload_scope,
                                   const std::string& sensitivity)
{
    wikore::rag::ChunkPayload p;
    p.company_id          = CO;
    p.document_version_id = r.version_id;
    p.chunk_id            = r.chunk_id;
    p.access_scope_ids    = payload_scope;
    p.sensitivity_label   = sensitivity;
    p.lifecycle_status    = "active";
    return {.id = "pt-" + r.chunk_id, .vector = wikore::rag::Embedding(DIM, 0.1f),
            .payload = std::move(p)};
}

} // namespace

TEST_CASE("RetrievalOrchestrator: gate overrides a stale prefilter and clearance excludes restricted",
          "[integration][orchestrator]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get(); seed(db);
    const auto A = make_ou(db, g_root, "a");
    const auto B = make_ou(db, g_root, "b");

    // reader is a member of A.
    const auto user = std::string(exec_sync(db,
        "INSERT INTO users (company_id,external_sub,email) "
        "VALUES ($1::uuid,'subO','o@test') RETURNING id", std::string(CO))[0]["id"].c_str());
    exec_sync(db,
        "INSERT INTO memberships (company_id,user_id,org_unit_id,role,applies_to) "
        "VALUES ($1::uuid,$2::uuid,$3::uuid,'viewer','self_and_descendants')",
        std::string(CO), user, A);

    auto visible   = make_chunk(db, A, "internal");   // owner A -> visible
    auto stale     = make_chunk(db, B, "internal");   // owner B, but payload will claim A
    auto restricted= make_chunk(db, A, "restricted"); // owner A but above clearance

    auto store = std::make_shared<wikore::rag::NullVectorStore>();
    drogon::sync_wait(store->upsert(std::vector<wikore::rag::UpsertPoint>{
        point_for(visible,    {A}, "internal"),
        point_for(stale,      {A}, "internal"),   // STALE payload scope (live owner is B)
        point_for(restricted, {A}, "restricted"),
    }));

    wikore::rag::RetrievalOrchestrator orch(
        std::make_shared<wikore::rag::NullEmbedder>(DIM),
        std::make_shared<wikore::PostgresAccessResolver>(db),
        store,
        wikore::rag::EvidenceGate(db));

    wikore::RequestContext ctx{
        .tenant    = {.company_id = CO},
        .principal = {.user_id = user, .email = "o@test"},
        .span      = {},
        .deadline  = std::chrono::steady_clock::now() + std::chrono::seconds(30),
    };

    auto r = drogon::sync_wait(orch.retrieve(ctx, "any query", g_root, 10));
    REQUIRE(r.has_value());

    std::vector<std::string> got;
    for (const auto& a : *r) got.push_back(a.chunk_id());

    // Only the genuinely-visible chunk survives:
    //  - `visible`    : prefilter pass + gate allow  -> IN
    //  - `stale`      : prefilter pass (stale scope) but gate DENIES (live owner B) -> OUT
    //  - `restricted` : prefilter DROPS it (clearance excludes 'restricted')        -> OUT
    REQUIRE(r->size() == 1);
    CHECK(got[0] == visible.chunk_id);
    CHECK(std::find(got.begin(), got.end(), stale.chunk_id)      == got.end());
    CHECK(std::find(got.begin(), got.end(), restricted.chunk_id) == got.end());
    CHECK((*r)[0].text().find("body") != std::string::npos);  // hydrated by the gate

    // P2: a very large limit must not overflow limit * over_fetch; the result
    // stays bounded and correct.
    auto big = drogon::sync_wait(orch.retrieve(ctx, "any query", g_root, 2000000000));
    REQUIRE(big.has_value());
    CHECK(big->size() == 1);
}

TEST_CASE("RetrievalOrchestrator: a non-positive limit is rejected before any work (P2)",
          "[integration][orchestrator]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get(); seed(db);
    wikore::rag::RetrievalOrchestrator orch(
        std::make_shared<wikore::rag::NullEmbedder>(DIM),
        std::make_shared<wikore::PostgresAccessResolver>(db),
        std::make_shared<wikore::rag::NullVectorStore>(),
        wikore::rag::EvidenceGate(db));
    wikore::RequestContext ctx{
        .tenant = {.company_id = CO}, .principal = {.user_id = "u", .email = "x"},
        .span = {}, .deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30)};

    for (int bad : {0, -5}) {
        auto r = drogon::sync_wait(orch.retrieve(ctx, "q", g_root, bad));
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().kind == wikore::Error::Kind::InvalidInput);
    }
}
