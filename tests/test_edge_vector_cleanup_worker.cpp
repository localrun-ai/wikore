#include <catch2/catch_test_macros.hpp>
#include "wikore/scheduler/edge_vector_cleanup_worker.hpp"
#include "wikore/rag/vector_store.hpp"
#include "wikore/db.hpp"
#include <drogon/drogon.h>
#include <drogon/utils/coroutine.h>
#include <atomic>
#include <cstdlib>
#include <memory>
#include <string>

// ---------------------------------------------------------------------------
// EdgeVectorCleanupWorker integration tests.
//
// Exercises the qdrant_delete_edge_points path: V034's BEFORE DELETE trigger
// on knowledge_edges captures every qdrant_point_id of the outgoing edge's
// embeddings into an outbox event (before ON DELETE CASCADE removes the
// child rows). The worker claims the event, calls
// VectorStorePort::delete_points_by_id, and marks the event completed.
//
// The trigger and payload shape are covered by db/smoke_tests/V034; this
// test covers only the C++ consumer.
//
// Skip without DATABASE_URL. The DrogonLoop fixture in
// test_promote_version.cpp serves these tests (single loop per process).
// ---------------------------------------------------------------------------

namespace {

// Fixed IDs are safe for company/chunk/model — they have no reuse guard,
// and seed()'s `DELETE FROM companies` cascade wipes them cleanly across
// runs. The knowledge_edges UUID is NOT safe: V034's
// knowledge_edges_no_uuid_reuse trigger rejects any INSERT whose id has
// prior history, and knowledge_edges_history has no company FK by design,
// so the cascade cannot clear it. Every seed() below picks a fresh edge id.
constexpr auto CO_EDGECLEAN     = "ec000000-0000-0000-0000-0000000000c1";
constexpr auto DOC_EDGECLEAN    = "ec0d0000-0000-0000-0000-000000000001";
constexpr auto VER_EDGECLEAN    = "ec7e0000-0000-0000-0000-000000000001";
constexpr auto CH1_EDGECLEAN    = "ecc80000-0000-0000-0000-000000000001";
constexpr auto CH2_EDGECLEAN    = "ecc80000-0000-0000-0000-000000000002";
constexpr auto MODEL_EDGECLEAN  = "ec110000-0000-0000-0000-000000000001";

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

struct SeedIds {
    std::string edge_id;
    std::string point_id;
};

// Seed a company with two chunks, a fresh knowledge_edge, and one edge
// embedding row. Returns the fresh edge/point ids so each test operates on
// its own edge UUID (see comment above).
//
// Correctness note (was P1 in review): drogon's TransactionPtr commits
// on destruction ASYNCHRONOUSLY, so a sync_wait around a
// begin+INSERT+COMMIT coroutine returns before COMMIT actually lands —
// a following INSERT into knowledge_edge_embeddings then races the
// parent FK. We issue the whole atomic block as one multi-statement
// simple-protocol string (no bind params -> drogon uses simple query
// protocol; multi-statement allowed), which completes synchronously and
// surfaces commit-time trigger errors (e.g. the deferred two-endpoint
// check) directly to the test.
SeedIds seed(drogon::orm::DbClientPtr db)
{
    exec_sync(db, "DELETE FROM companies WHERE id=$1::uuid", std::string(CO_EDGECLEAN));
    exec_sync(db, "DELETE FROM embedding_models WHERE id=$1::uuid", std::string(MODEL_EDGECLEAN));

    exec_sync(db,
        "INSERT INTO companies (id,name,slug) VALUES ($1::uuid,'EdgeClean','edgeclean')",
        std::string(CO_EDGECLEAN));
    auto root = std::string(exec_sync(db,
        "SELECT id FROM org_units WHERE company_id=$1::uuid AND type='root'",
        std::string(CO_EDGECLEAN))[0]["id"].c_str());
    exec_sync(db,
        "INSERT INTO embedding_models (id,name,qdrant_collection,dimension) "
        "VALUES ($1::uuid,'edgeclean-model','wikore_edgeclean_test',4)",
        std::string(MODEL_EDGECLEAN));
    exec_sync(db,
        "INSERT INTO documents (id,company_id,owner_org_unit_id,filename,title,mime_type) "
        "VALUES ($1::uuid,$2::uuid,$3::uuid,'e.txt','E','text/plain')",
        std::string(DOC_EDGECLEAN), std::string(CO_EDGECLEAN), root);
    exec_sync(db,
        "INSERT INTO document_versions (id,company_id,document_id,version_no,source_hash,"
        "ingest_status,completed_at,activated_at,chunk_count,lifecycle_status) "
        "VALUES ($1::uuid,$2::uuid,$3::uuid,1,'h','done',now(),now(),2,'active')",
        std::string(VER_EDGECLEAN), std::string(CO_EDGECLEAN), std::string(DOC_EDGECLEAN));
    exec_sync(db,
        "INSERT INTO document_chunks (id,company_id,document_version_id,chunk_index,content,content_hash) "
        "VALUES ($1::uuid,$2::uuid,$3::uuid,0,'a','ha'),"
        "       ($4::uuid,$2::uuid,$3::uuid,1,'b','hb')",
        std::string(CH1_EDGECLEAN), std::string(CO_EDGECLEAN), std::string(VER_EDGECLEAN),
        std::string(CH2_EDGECLEAN));

    // Fresh UUIDs per test to sidestep V034's no-reuse guard.
    auto edge_id  = std::string(exec_sync(db, "SELECT gen_random_uuid() AS id")[0]["id"].c_str());
    auto point_id = std::string(exec_sync(db, "SELECT gen_random_uuid() AS id")[0]["id"].c_str());

    // Insert edge + endpoints as ONE data-modifying CTE.
    //
    // Correctness note: drogon's TransactionPtr commits on destruction
    // ASYNCHRONOUSLY, so a sync_wait around a begin+INSERT+COMMIT coroutine
    // returns before COMMIT actually lands — a following INSERT into
    // knowledge_edge_embeddings then races the parent FK. Multi-statement
    // "BEGIN; ...; COMMIT;" is also unusable here: the CI drogon build
    // enables libpq pipeline mode (PgBatchConnection), which routes every
    // query through a prepared statement, and prepared statements reject
    // multi-command strings ("cannot insert multiple commands into a
    // prepared statement").
    //
    // A single-statement data-modifying CTE runs in its own implicit
    // transaction: the deferred two-endpoint CONSTRAINT TRIGGER fires at
    // that statement's commit, and exec_sync returning means the commit
    // has landed — so the next INSERT into knowledge_edge_embeddings sees
    // the parent edge row without a race. Bind parameters are fine again
    // (single command).
    exec_sync(db,
        "WITH e AS ("
        "  INSERT INTO knowledge_edges "
        "    (id,company_id,edge_type,direction,confidence,origin) "
        "  VALUES ($1::uuid,$2::uuid,'implements','directed',0.9,'administrator') "
        "  RETURNING id, company_id"
        ") "
        "INSERT INTO knowledge_edge_endpoints (company_id,edge_id,ordinal,chunk_id,role) "
        "SELECT company_id, id, 0, $3::uuid, 'source' FROM e "
        "UNION ALL "
        "SELECT company_id, id, 1, $4::uuid, 'target' FROM e",
        edge_id, std::string(CO_EDGECLEAN),
        std::string(CH1_EDGECLEAN), std::string(CH2_EDGECLEAN));

    exec_sync(db,
        "INSERT INTO knowledge_edge_embeddings "
        "(company_id,edge_id,embedding_model_id,qdrant_point_id,formula_version,indexed_edge_version) "
        "VALUES ($1::uuid,$2::uuid,$3::uuid,$4::uuid,1,1)",
        std::string(CO_EDGECLEAN), edge_id, std::string(MODEL_EDGECLEAN), point_id);

    return {edge_id, point_id};
}

// Build a NullVectorStore pre-seeded with the edge point so we can observe
// its removal after the worker runs. company_id in the payload is set so
// the tenant-scoped delete_points_by_id filter actually matches.
std::shared_ptr<wikore::rag::NullVectorStore>
seeded_store(const std::string& point_id)
{
    auto store = std::make_shared<wikore::rag::NullVectorStore>();
    wikore::rag::ChunkPayload p;
    p.company_id = CO_EDGECLEAN;
    drogon::sync_wait(store->upsert(std::vector<wikore::rag::UpsertPoint>{
        {.id = point_id,
         .vector = wikore::rag::Embedding(4, 0.1f),
         .payload = std::move(p)}}));
    return store;
}

wikore::scheduler::EdgeVectorCleanupWorker make_worker(
    drogon::orm::DbClientPtr db,
    std::shared_ptr<wikore::rag::VectorStorePort> store,
    std::atomic<bool>& stop)
{
    return wikore::scheduler::EdgeVectorCleanupWorker(
        db, store, [&] { return stop.load(); },
        wikore::scheduler::EdgeVectorCleanupWorker::Options{});
}

} // namespace

TEST_CASE("EdgeVectorCleanupWorker: deleting an edge triggers point removal",
          "[integration][edge_cleanup]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    auto ids = seed(db);
    auto store = seeded_store(ids.point_id);
    REQUIRE(store->contains(ids.point_id));

    // Deleting the edge fires the V034 BEFORE DELETE trigger, which
    // enqueues one qdrant_delete_edge_points event containing the point id.
    exec_sync(db, "DELETE FROM knowledge_edges WHERE id=$1::uuid", ids.edge_id);

    // Sanity: the outbox event exists and is unclaimed.
    auto pre = exec_sync(db,
        "SELECT id, payload->'qdrant_point_ids' AS pids "
        "FROM outbox_events "
        "WHERE aggregate_id=$1::uuid AND job_type='qdrant_delete_edge_points' "
        "  AND completed_at IS NULL",
        ids.edge_id);
    REQUIRE(pre.size() == 1);

    std::atomic<bool> stop{false};
    auto worker = make_worker(db, store, stop);
    // drain_once is a GLOBAL drainer (job_type only). Assert on THIS edge's
    // outcome, not the aggregate counters.
    int processed = drogon::sync_wait(worker.drain_once());
    CHECK(processed >= 1);

    // Outbox event completed.
    auto post = exec_sync(db,
        "SELECT completed_at IS NOT NULL AS done "
        "FROM outbox_events "
        "WHERE aggregate_id=$1::uuid AND job_type='qdrant_delete_edge_points'",
        ids.edge_id);
    REQUIRE(post.size() == 1);
    CHECK(std::string(post[0]["done"].c_str()) == "t");

    // Qdrant point is gone.
    CHECK_FALSE(store->contains(ids.point_id));
}

TEST_CASE("EdgeVectorCleanupWorker: redelivery of the same event is idempotent",
          "[integration][edge_cleanup]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    auto ids = seed(db);
    auto store = seeded_store(ids.point_id);

    // Trigger the outbox event.
    exec_sync(db, "DELETE FROM knowledge_edges WHERE id=$1::uuid", ids.edge_id);

    std::atomic<bool> stop{false};
    auto worker = make_worker(db, store, stop);

    // First drain: the point exists, gets deleted, event marked completed.
    int processed = drogon::sync_wait(worker.drain_once());
    CHECK(processed >= 1);
    CHECK_FALSE(store->contains(ids.point_id));

    // Simulate redelivery of the same completed event: rewind it to
    // pending and drain again. delete_points_by_id over a non-existent
    // id must be a clean no-op (matches Qdrant filter semantics).
    exec_sync(db,
        "UPDATE outbox_events "
        "SET completed_at=NULL, claimed_at=NULL, claimed_by=NULL "
        "WHERE aggregate_id=$1::uuid AND job_type='qdrant_delete_edge_points'",
        ids.edge_id);
    processed = drogon::sync_wait(worker.drain_once());
    CHECK(processed >= 1);
    auto post = exec_sync(db,
        "SELECT completed_at IS NOT NULL AS done "
        "FROM outbox_events "
        "WHERE aggregate_id=$1::uuid AND job_type='qdrant_delete_edge_points'",
        ids.edge_id);
    CHECK(std::string(post[0]["done"].c_str()) == "t");
}

TEST_CASE("EdgeVectorCleanupWorker: cross-tenant point IDs are not deleted",
          "[integration][edge_cleanup]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    auto ids = seed(db);

    // Store contains ONE point that claims to belong to a DIFFERENT tenant.
    // delete_points_by_id(CO_EDGECLEAN, [that_point]) must NOT remove it —
    // the has_id + company_id filter combination rules it out (regression
    // test for the P3 tenant-scoping finding on the port).
    auto store = std::make_shared<wikore::rag::NullVectorStore>();
    wikore::rag::ChunkPayload p;
    p.company_id = "ffffffff-ffff-ffff-ffff-ffffffffffff";  // some other tenant
    drogon::sync_wait(store->upsert(std::vector<wikore::rag::UpsertPoint>{
        {.id = ids.point_id,
         .vector = wikore::rag::Embedding(4, 0.1f),
         .payload = std::move(p)}}));

    exec_sync(db, "DELETE FROM knowledge_edges WHERE id=$1::uuid", ids.edge_id);

    std::atomic<bool> stop{false};
    auto worker = make_worker(db, store, stop);
    int processed = drogon::sync_wait(worker.drain_once());
    CHECK(processed >= 1);

    // The point survives even though its id was in the event's payload,
    // because the filter's company_id clause did not match. The event
    // still completes — from the worker's perspective the requested
    // delete succeeded (Qdrant filter no-op).
    CHECK(store->contains(ids.point_id));
    auto post = exec_sync(db,
        "SELECT completed_at IS NOT NULL AS done "
        "FROM outbox_events "
        "WHERE aggregate_id=$1::uuid AND job_type='qdrant_delete_edge_points'",
        ids.edge_id);
    CHECK(std::string(post[0]["done"].c_str()) == "t");
}
