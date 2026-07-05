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

constexpr auto CO_EDGECLEAN     = "ec000000-0000-0000-0000-0000000000c1";
constexpr auto DOC_EDGECLEAN    = "ec0d0000-0000-0000-0000-000000000001";
constexpr auto VER_EDGECLEAN    = "ec7e0000-0000-0000-0000-000000000001";
constexpr auto CH1_EDGECLEAN    = "ecc80000-0000-0000-0000-000000000001";
constexpr auto CH2_EDGECLEAN    = "ecc80000-0000-0000-0000-000000000002";
constexpr auto MODEL_EDGECLEAN  = "ec110000-0000-0000-0000-000000000001";
constexpr auto EDGE_EDGECLEAN   = "ec6c0000-0000-0000-0000-000000000001";
constexpr auto POINT_EDGECLEAN  = "ecff0000-0000-0000-0000-000000000001";

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

// Seed a company with two chunks, one knowledge_edge between them, and one
// edge embedding row referencing POINT_EDGECLEAN. Deleting the edge fires the
// V034 trigger which enqueues an outbox event carrying [POINT_EDGECLEAN].
void seed(drogon::orm::DbClientPtr db)
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

    // Insert edge + endpoints atomically so the deferred CONSTRAINT TRIGGER
    // passes at COMMIT.
    drogon::sync_wait([db]() -> drogon::Task<void> {
        auto tx = co_await db->newTransactionCoro();
        co_await tx->execSqlCoro(
            "INSERT INTO knowledge_edges (id,company_id,edge_type,direction,confidence,origin) "
            "VALUES ($1::uuid,$2::uuid,'implements','directed',0.9,'administrator')",
            std::string(EDGE_EDGECLEAN), std::string(CO_EDGECLEAN));
        co_await tx->execSqlCoro(
            "INSERT INTO knowledge_edge_endpoints (company_id,edge_id,ordinal,chunk_id,role) "
            "VALUES ($1::uuid,$2::uuid,0,$3::uuid,'source'),"
            "       ($1::uuid,$2::uuid,1,$4::uuid,'target')",
            std::string(CO_EDGECLEAN), std::string(EDGE_EDGECLEAN),
            std::string(CH1_EDGECLEAN), std::string(CH2_EDGECLEAN));
    }());
    exec_sync(db,
        "INSERT INTO knowledge_edge_embeddings "
        "(company_id,edge_id,embedding_model_id,qdrant_point_id,formula_version,indexed_edge_version) "
        "VALUES ($1::uuid,$2::uuid,$3::uuid,$4::uuid,1,1)",
        std::string(CO_EDGECLEAN), std::string(EDGE_EDGECLEAN),
        std::string(MODEL_EDGECLEAN), std::string(POINT_EDGECLEAN));
}

// Build a NullVectorStore pre-seeded with the edge point so we can observe
// its removal after the worker runs.
std::shared_ptr<wikore::rag::NullVectorStore> seeded_store()
{
    auto store = std::make_shared<wikore::rag::NullVectorStore>();
    wikore::rag::ChunkPayload p;
    p.company_id = CO_EDGECLEAN;
    drogon::sync_wait(store->upsert(std::vector<wikore::rag::UpsertPoint>{
        {.id = POINT_EDGECLEAN,
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
    seed(db);
    auto store = seeded_store();
    REQUIRE(store->contains(POINT_EDGECLEAN));

    // Deleting the edge fires the V034 BEFORE DELETE trigger, which
    // enqueues one qdrant_delete_edge_points event containing the point id.
    exec_sync(db, "DELETE FROM knowledge_edges WHERE id=$1::uuid",
              std::string(EDGE_EDGECLEAN));

    // Sanity: the outbox event exists and is unclaimed.
    auto pre = exec_sync(db,
        "SELECT id, payload->'qdrant_point_ids' AS pids "
        "FROM outbox_events "
        "WHERE aggregate_id=$1::uuid AND job_type='qdrant_delete_edge_points' "
        "  AND completed_at IS NULL",
        std::string(EDGE_EDGECLEAN));
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
        std::string(EDGE_EDGECLEAN));
    REQUIRE(post.size() == 1);
    CHECK(std::string(post[0]["done"].c_str()) == "t");

    // Qdrant point is gone.
    CHECK_FALSE(store->contains(POINT_EDGECLEAN));
}

TEST_CASE("EdgeVectorCleanupWorker: redelivery of the same event is idempotent",
          "[integration][edge_cleanup]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    seed(db);
    auto store = seeded_store();

    // Trigger the outbox event.
    exec_sync(db, "DELETE FROM knowledge_edges WHERE id=$1::uuid",
              std::string(EDGE_EDGECLEAN));

    // Rewind: mark the event as unclaimed and not yet completed so a second
    // drain sees it again. This simulates a retry after an unclean shutdown
    // between delete_points_by_id and mark_completed.
    exec_sync(db,
        "UPDATE outbox_events "
        "SET completed_at=NULL, claimed_at=NULL, claimed_by=NULL "
        "WHERE aggregate_id=$1::uuid AND job_type='qdrant_delete_edge_points'",
        std::string(EDGE_EDGECLEAN));

    std::atomic<bool> stop{false};
    auto worker = make_worker(db, store, stop);

    // First drain: the point exists, gets deleted.
    (void)drogon::sync_wait(worker.drain_once());
    CHECK_FALSE(store->contains(POINT_EDGECLEAN));

    // Simulate redelivery: reopen the event, run again. delete_points_by_id
    // over a non-existent id must be a clean no-op (matches Qdrant semantics).
    exec_sync(db,
        "UPDATE outbox_events "
        "SET completed_at=NULL, claimed_at=NULL, claimed_by=NULL "
        "WHERE aggregate_id=$1::uuid AND job_type='qdrant_delete_edge_points'",
        std::string(EDGE_EDGECLEAN));
    int processed = drogon::sync_wait(worker.drain_once());
    CHECK(processed >= 1);
    auto post = exec_sync(db,
        "SELECT completed_at IS NOT NULL AS done "
        "FROM outbox_events "
        "WHERE aggregate_id=$1::uuid AND job_type='qdrant_delete_edge_points'",
        std::string(EDGE_EDGECLEAN));
    CHECK(std::string(post[0]["done"].c_str()) == "t");
}
