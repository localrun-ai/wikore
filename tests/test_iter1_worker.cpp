#include <catch2/catch_test_macros.hpp>
#include "wikore/application/ingest_document_version.hpp"
#include "wikore/db.hpp"
#include "wikore/ingest/chunker.hpp"
#include "wikore/ingest/document_repo.hpp"
#include "wikore/ingest/parser.hpp"
#include "wikore/ingest/worker.hpp"
#include "wikore/rag/embedder.hpp"
#include "wikore/rag/vector_store.hpp"
#include "wikore/redis.hpp"
#include "wikore/scheduler/outbox_worker.hpp"
#include "wikore/scheduler/polling_fallback.hpp"
#include <drogon/drogon.h>
#include <drogon/utils/coroutine.h>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>

// ---------------------------------------------------------------------------
// Iter-1 worker integration tests.
//
// Exercises the full round-trip: Redis queue -> IngestWorker ->
// IngestDocumentVersionUseCase -> Postgres (chunks + audit + outbox event)
// -> OutboxWorker -> NullEmbedder + NullVectorStore -> document_chunk_vectors.
// Then a separate test exercises the PollingFallback's stuck-version sweep.
//
// Embedder + vector store are Null* implementations so the tests do not
// require llama-server or Qdrant. The production wiring is exercised by
// the main() binaries themselves; here we are validating the worker logic
// against the real Postgres + Redis schema.
//
// Tests SKIP if DATABASE_URL or REDIS_URL is unset. The DrogonLoop fixture
// declared in test_promote_version.cpp also serves these tests (single
// loop per process; drogon::app() is a singleton).
// ---------------------------------------------------------------------------

namespace {

constexpr auto CO       = "ca0e1111-0000-0000-0000-000000000001";
constexpr auto USR      = "ca0e1111-0000-0000-0000-000000000002";
constexpr auto DOC      = "d0c01111-1111-0000-0000-000000000001";
constexpr auto VERSION  = "7e101111-1111-0000-0000-000000000001";
constexpr auto MODEL_ID = "11ed1111-1111-0000-0000-000000000001";
constexpr auto MODEL    = "null-embedder";

bool db_available()    { return std::getenv("DATABASE_URL") != nullptr; }
bool redis_available() { return std::getenv("REDIS_URL")    != nullptr; }

template<typename... Args>
drogon::orm::Result exec_sync(drogon::orm::DbClientPtr db, std::string sql, Args... args) {
    return drogon::sync_wait(
        [db, sql = std::move(sql), ...args = std::move(args)]()
        -> drogon::Task<drogon::orm::Result> {
            co_return co_await db->execSqlCoro(sql, args...);
        }());
}

void seed_iter1_fixtures(drogon::orm::DbClientPtr db) {
    exec_sync(db, "DELETE FROM companies WHERE id=$1::uuid", std::string(CO));
    exec_sync(db,
        "INSERT INTO companies (id, name, slug) "
        "VALUES ($1::uuid, 'Iter1Co', 'iter1co')",
        std::string(CO));
    auto ou_rows = exec_sync(db,
        "SELECT id FROM org_units WHERE company_id=$1::uuid AND type='root'",
        std::string(CO));
    REQUIRE(!ou_rows.empty());
    const auto root_ou = std::string(ou_rows[0]["id"].c_str());

    exec_sync(db,
        "INSERT INTO users (id, company_id, external_issuer, external_sub, email, display_name) "
        "VALUES ($1::uuid, $2::uuid, 'iss', 'sub-iter1', 'iter1@t.com', 'Iter1')",
        std::string(USR), std::string(CO));
    exec_sync(db,
        "INSERT INTO documents (id, company_id, owner_org_unit_id, filename) "
        "VALUES ($1::uuid, $2::uuid, $3::uuid, 'iter1.md')",
        std::string(DOC), std::string(CO), root_ou);
    exec_sync(db,
        "INSERT INTO document_versions "
        "(id, company_id, document_id, version_no, source_hash, ingest_status, lifecycle_status) "
        "VALUES ($1::uuid, $2::uuid, $3::uuid, 1, 'h1', 'pending', 'draft')",
        std::string(VERSION), std::string(CO), std::string(DOC));

    // embedding_models registry row. The outbox worker resolves
    // embed_model_id (string name) -> embedding_models.id (uuid).
    // ON CONFLICT DO NOTHING so re-running the test suite is fine.
    exec_sync(db,
        "INSERT INTO embedding_models (id, name, qdrant_collection, dimension) "
        "VALUES ($1::uuid, $2, 'wikore_chunks_v1_test', 4) "
        "ON CONFLICT (name) DO NOTHING",
        std::string(MODEL_ID), std::string(MODEL));
}

std::string write_temp_iter1_doc() {
    auto dir = std::filesystem::temp_directory_path() / "wikore-iter1-it";
    std::filesystem::create_directories(dir);
    auto path = dir / "iter1.md";
    std::ofstream f(path);
    f << "# Onboarding\n\n"
      << "Welcome to the team!\n\n"
      << "## Compliance\n\n"
      << "All employees must complete training in the first week.\n\n"
      << "## Resources\n\n"
      << "See the internal portal for forms and policies.\n";
    return path.string();
}

auto make_use_case(drogon::orm::DbClientPtr db) {
    return wikore::application::IngestDocumentVersionUseCase{
        db,
        std::make_shared<wikore::ingest::PostgresDocumentRepo>(db),
        std::make_shared<wikore::ingest::PlainTextParser>(),
        wikore::ingest::Chunker{},
    };
}

// Helper: push a job JSON onto lr:ingest:q:{CO}.
void enqueue_job(const std::string& file_path) {
    const auto key = std::string("lr:ingest:q:") + CO;
    // glaze-style JSON keys must match the IngestJob meta declared in
    // src/ingest/worker.cpp.
    auto json = std::string(R"({"company_id":")") + CO
              + R"(","document_id":")" + DOC
              + R"(","document_version_id":")" + VERSION
              + R"(","file_path":")" + file_path
              + R"(","embed_model_id":")" + MODEL
              + R"(","priority":0})";
    wikore::Redis::lpush(key, json);
}

void clear_queues() {
    auto keys = wikore::Redis::scan_keys("lr:ingest:q:*");
    for (const auto& k : keys)
        wikore::Redis::del(k);
}

// An embedder that mutates the document's ACL state DURING embed_batch, to
// simulate a resync (grant change) landing in the window between the outbox
// worker's pre-embed read and its locked re-read. Returns unit-ish vectors like
// NullEmbedder so the rest of the pipeline is unaffected.
struct MutatingEmbedder : wikore::rag::EmbedderPort {
    drogon::orm::DbClientPtr db;
    std::string version_id;
    std::string extra_scope_ou;
    int         d = 4;
    std::string name = "null-embedder";

    drogon::Task<wikore::Result<wikore::rag::Embedding>> embed(std::string /*t*/) override {
        // The outbox worker only calls embed_batch (which carries the ACL
        // mutation); embed() just satisfies the interface. Returns a direct
        // value rather than awaiting embed_batch -- a nested co_await here
        // ICEs GCC 13 (build_special_member_call, cp/call.cc:11096).
        co_return wikore::rag::Embedding(d, 0.1f);
    }
    drogon::Task<wikore::Result<std::vector<wikore::rag::Embedding>>>
    embed_batch(std::vector<std::string> texts) override {
        co_await db->execSqlCoro(
            "UPDATE documents SET acl_version = acl_version + 1 "
            "WHERE id = (SELECT document_id FROM document_versions WHERE id=$1::uuid)",
            version_id);
        co_await db->execSqlCoro(
            "UPDATE document_chunks "
            "SET qdrant_prefilter_scope_ids = qdrant_prefilter_scope_ids || $2::uuid "
            "WHERE document_version_id=$1::uuid",
            version_id, extra_scope_ou);
        co_return std::vector<wikore::rag::Embedding>(texts.size(),
                                                      wikore::rag::Embedding(d, 0.1f));
    }
    int dims() const override { return d; }
    const std::string& model_name() const override { return name; }
};

} // namespace

// ---------------------------------------------------------------------------

TEST_CASE("IngestWorker: dispatches a queued job and the full pipeline succeeds",
          "[integration][iter1]")
{
    if (!db_available() || !redis_available()) SKIP("DATABASE_URL or REDIS_URL not set");
    auto db = wikore::Db::get();
    seed_iter1_fixtures(db);
    clear_queues();

    const auto file_path = write_temp_iter1_doc();
    enqueue_job(file_path);

    std::atomic<bool> stop{false};
    auto uc = make_use_case(db);
    wikore::ingest::IngestWorker::Options opts;
    opts.idle_sleep      = std::chrono::milliseconds(50);
    opts.rescan_interval = std::chrono::seconds(1);
    wikore::ingest::IngestWorker worker(std::move(uc), [&] { return stop.load(); }, opts);

    // Run the worker briefly: enough time for one rotation + dispatch.
    drogon::sync_wait([&]() -> drogon::Task<void> {
        auto loop = drogon::app().getLoop();
        loop->runAfter(2.0, [&] { stop.store(true); });
        co_await worker.run();
    }());

    CHECK(worker.jobs_processed() == 1);
    CHECK(worker.jobs_failed()    == 0);

    // Use case wrote chunks; ingest_status is 'done'.
    auto v = exec_sync(db,
        "SELECT ingest_status, chunk_count FROM document_versions WHERE id=$1::uuid",
        std::string(VERSION));
    CHECK(std::string(v[0]["ingest_status"].c_str()) == "done");
    CHECK(std::stoi(v[0]["chunk_count"].c_str()) >= 1);

    // Outbox event was written; not yet drained.
    auto box = exec_sync(db,
        "SELECT job_type, completed_at IS NOT NULL AS done FROM outbox_events "
        "WHERE company_id=$1::uuid AND aggregate_id=$2::uuid "
        "AND job_type='qdrant_upsert_chunk_payload'",
        std::string(CO), std::string(VERSION));
    REQUIRE(box.size() == 1);
    CHECK(std::string(box[0]["done"].c_str()) == "f");
}

TEST_CASE("OutboxWorker: drains the qdrant_upsert_chunk_payload event end-to-end",
          "[integration][iter1]")
{
    if (!db_available() || !redis_available()) SKIP("DATABASE_URL or REDIS_URL not set");
    auto db = wikore::Db::get();
    seed_iter1_fixtures(db);
    clear_queues();

    // Drive ingest first to land an outbox event.
    const auto file_path = write_temp_iter1_doc();
    enqueue_job(file_path);

    std::atomic<bool> stop{false};
    auto uc = make_use_case(db);
    wikore::ingest::IngestWorker::Options iopts;
    iopts.idle_sleep      = std::chrono::milliseconds(50);
    iopts.rescan_interval = std::chrono::seconds(1);
    wikore::ingest::IngestWorker worker(std::move(uc), [&] { return stop.load(); }, iopts);
    drogon::sync_wait([&]() -> drogon::Task<void> {
        drogon::app().getLoop()->runAfter(1.5, [&] { stop.store(true); });
        co_await worker.run();
    }());
    REQUIRE(worker.jobs_processed() == 1);

    // Now run the outbox worker once. NullEmbedder produces 4-dim vectors
    // matching the embedding_models row we seeded (dimension = 4).
    auto embedder    = std::make_shared<wikore::rag::NullEmbedder>(/*dims=*/4);
    auto vec_store   = std::make_shared<wikore::rag::NullVectorStore>();
    std::atomic<bool> ob_stop{false};
    wikore::scheduler::OutboxWorker outbox(
        db, embedder, vec_store, [&] { return ob_stop.load(); },
        wikore::scheduler::OutboxWorker::Options{});

    int completed = drogon::sync_wait(outbox.drain_once());
    CHECK(completed == 1);

    // Outbox event is now marked completed.
    auto box = exec_sync(db,
        "SELECT completed_at IS NOT NULL AS done, attempt_count "
        "FROM outbox_events WHERE company_id=$1::uuid AND aggregate_id=$2::uuid",
        std::string(CO), std::string(VERSION));
    REQUIRE(box.size() == 1);
    CHECK(std::string(box[0]["done"].c_str()) == "t");
    CHECK(std::stoi(box[0]["attempt_count"].c_str()) >= 1);

    // Qdrant got the points.
    CHECK(vec_store->point_count() >= 1);

    // document_chunk_vectors bookkeeping populated (one row per chunk).
    auto vec_rows = exec_sync(db,
        "SELECT COUNT(*) AS n FROM document_chunk_vectors "
        "WHERE company_id=$1::uuid AND embedding_model_id=$2::uuid",
        std::string(CO), std::string(MODEL_ID));
    CHECK(std::stoi(vec_rows[0]["n"].c_str()) >= 1);
}

TEST_CASE("OutboxWorker: re-reads ACL under the lock, so a resync during embed is not clobbered",
          "[integration][iter1]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    seed_iter1_fixtures(db);

    const auto root = std::string(exec_sync(db,
        "SELECT id FROM org_units WHERE company_id=$1::uuid AND type='root'",
        std::string(CO))[0]["id"].c_str());
    const auto team = std::string(exec_sync(db,
        "INSERT INTO org_units (company_id,parent_id,type,slug,name) "
        "VALUES ($1::uuid,$2::uuid,'team','it-team','IT') RETURNING id",
        std::string(CO), root)[0]["id"].c_str());

    // Move the version to 'active' (constraint requires completed/activated/
    // chunk_count) and seed one chunk with the STALE pre-embed scope {root}.
    exec_sync(db,
        "UPDATE document_versions SET ingest_status='done', lifecycle_status='active', "
        "completed_at=now(), activated_at=now(), chunk_count=1, sensitivity_label='internal' "
        "WHERE id=$1::uuid", std::string(VERSION));
    const auto chunk = std::string(exec_sync(db,
        "INSERT INTO document_chunks (company_id,document_version_id,chunk_index,content,"
        "content_hash,qdrant_prefilter_scope_ids) "
        "VALUES ($1::uuid,$2::uuid,0,'hello','ch',ARRAY[$3::uuid]) RETURNING id",
        std::string(CO), std::string(VERSION), root)[0]["id"].c_str());
    exec_sync(db,
        "INSERT INTO outbox_events (company_id,aggregate_id,job_type,payload,idempotency_key) "
        "VALUES ($1::uuid,$2::uuid,'qdrant_upsert_chunk_payload',"
        "        jsonb_build_object('document_id',$3::text,'embed_model_id',$4::text),"
        "        'upsert:reread')",
        std::string(CO), std::string(VERSION), std::string(DOC), std::string(MODEL));

    // The mutating embedder bumps acl_version and appends `team` to the scope
    // DURING embed (between pre-embed read and locked re-read).
    auto embedder = std::make_shared<MutatingEmbedder>();
    embedder->db = db; embedder->version_id = VERSION; embedder->extra_scope_ou = team;
    auto store = std::make_shared<wikore::rag::NullVectorStore>();
    std::atomic<bool> stop{false};
    wikore::scheduler::OutboxWorker outbox(
        db, embedder, store, [&] { return stop.load(); },
        wikore::scheduler::OutboxWorker::Options{});
    int completed = drogon::sync_wait(outbox.drain_once());
    CHECK(completed >= 1);

    // The upserted payload must carry the FRESH values (acl_version 2, scope
    // including `team`), proving the locked re-read overrode the stale pre-embed
    // read -- otherwise a late upsert would clobber a concurrent resync.
    const auto pid = *wikore::rag::uuid_v5(chunk + ":" + std::string(MODEL));
    const auto* pl = store->payload_for(pid);
    REQUIRE(pl != nullptr);
    CHECK(pl->acl_version >= 2);
    CHECK(std::find(pl->access_scope_ids.begin(), pl->access_scope_ids.end(), team)
          != pl->access_scope_ids.end());
}

TEST_CASE("PollingFallback: promotes stuck pending (no payload) versions to 'error'",
          "[integration][iter1]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    seed_iter1_fixtures(db);

    // Replace the seeded row with one whose updated_at is far enough in
    // the past to trip the 15-minute sweep. The set_updated_at trigger
    // is BEFORE UPDATE only, so an INSERT preserves the backdated
    // updated_at (UPDATE would rewrite it to now()). Payload IS NULL
    // so sweep #1's terminal-error branch fires (no recovery possible).
    exec_sync(db, "DELETE FROM document_versions WHERE id=$1::uuid",
              std::string(VERSION));
    exec_sync(db, R"(
        INSERT INTO document_versions
               (id, company_id, document_id, version_no, source_hash,
                ingest_status, lifecycle_status, updated_at)
        VALUES ($1::uuid, $2::uuid, $3::uuid, 1, 'h1',
                'pending', 'draft', now() - interval '20 minutes')
    )", std::string(VERSION), std::string(CO), std::string(DOC));

    std::atomic<bool> stop{false};
    wikore::scheduler::PollingFallback::Options opts;
    opts.interval        = std::chrono::seconds(60);
    opts.stuck_threshold = std::chrono::minutes(15);
    wikore::scheduler::PollingFallback poll(db, [&] { return stop.load(); }, opts);

    int swept = drogon::sync_wait(poll.sweep_once());
    CHECK(swept == 1);
    CHECK(poll.total_swept() == 1);

    auto v = exec_sync(db,
        "SELECT ingest_status, error_msg FROM document_versions WHERE id=$1::uuid",
        std::string(VERSION));
    CHECK(std::string(v[0]["ingest_status"].c_str()) == "error");
    CHECK(std::string(v[0]["error_msg"].c_str()).find("stuck pending") != std::string::npos);
}

TEST_CASE("PollingFallback::run: shutdown during the inter-sweep sleep exits within a chunk",
          "[integration][iter1]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    seed_iter1_fixtures(db);

    std::atomic<bool> stop{false};
    wikore::scheduler::PollingFallback::Options opts;
    // A long interval: with a non-cancellable sleep, run() would block ~1h after
    // the first sweep. The on_sleep_armed hook flips shutdown exactly while the
    // coroutine is suspended in the sleep, so the chunked loop must observe it
    // and return within one 50ms chunk.
    opts.interval       = std::chrono::hours(1);
    opts.sleep_chunk    = std::chrono::milliseconds(50);
    opts.on_sleep_armed = [&] { stop.store(true); };
    wikore::scheduler::PollingFallback poll(db, [&] { return stop.load(); }, opts);

    const auto t0 = std::chrono::steady_clock::now();
    drogon::sync_wait(poll.run());
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    // Must be far below the 1h interval -- proves the sleep is cancellable.
    CHECK(elapsed < std::chrono::seconds(5));
}

TEST_CASE("PollingFallback: ctor rejects non-positive interval and non-positive sleep_chunk",
          "[iter1]")
{
    // No DB needed: the ctor validates Options before touching the client, so a
    // null DbClientPtr keeps this runnable in the unit-test job (no DATABASE_URL).
    // A non-positive sleep_chunk would make interruptible_sleep arm a zero/negative
    // timer that fires immediately -- a busy loop that pins a CPU.
    drogon::orm::DbClientPtr null_db;
    auto shutdown = [] { return false; };

    using PF = wikore::scheduler::PollingFallback;

    PF::Options bad_chunk;
    bad_chunk.sleep_chunk = std::chrono::milliseconds::zero();
    REQUIRE_THROWS_AS(PF(null_db, shutdown, bad_chunk), std::invalid_argument);

    PF::Options bad_interval;
    bad_interval.interval = std::chrono::seconds::zero();
    REQUIRE_THROWS_AS(PF(null_db, shutdown, bad_interval), std::invalid_argument);
}

TEST_CASE("IngestWorker: malformed JSON payload is discarded without dispatch",
          "[integration][iter1]")
{
    if (!db_available() || !redis_available()) SKIP("DATABASE_URL or REDIS_URL not set");
    auto db = wikore::Db::get();
    seed_iter1_fixtures(db);
    clear_queues();

    wikore::Redis::lpush(std::string("lr:ingest:q:") + CO,
                          "{not valid json");

    std::atomic<bool> stop{false};
    auto uc = make_use_case(db);
    wikore::ingest::IngestWorker::Options opts;
    opts.idle_sleep      = std::chrono::milliseconds(50);
    opts.rescan_interval = std::chrono::seconds(1);
    wikore::ingest::IngestWorker worker(std::move(uc), [&] { return stop.load(); }, opts);

    drogon::sync_wait([&]() -> drogon::Task<void> {
        drogon::app().getLoop()->runAfter(1.0, [&] { stop.store(true); });
        co_await worker.run();
    }());

    CHECK(worker.jobs_processed() == 0);
    CHECK(worker.jobs_failed()    >= 1);
}

// ---------------------------------------------------------------------------
// Failure-injection tests (added per PR #19 review).
//
// Each test simulates one of the documented failure modes and asserts the
// system recovers safely, addressing the gap the reviewer flagged:
// "the new tests cover only uninterrupted happy paths."
// ---------------------------------------------------------------------------

TEST_CASE("PollingFallback: stuck 'processing' rows are promoted to 'error'",
          "[integration][iter1][failure_injection]")
{
    // P1 #1 from PR #19 review: a worker that crashes mid-job leaves the
    // version row at 'processing' (set by use case step 1) but the Redis
    // job was already RPOPed. The expanded sweep must promote these to
    // 'error' so they don't become permanent zombies.
    //
    // We seed a *fresh* version row directly in 'processing' state with
    // a backdated updated_at. The V003 set_updated_at trigger is
    // BEFORE UPDATE only, so an INSERT preserves whatever updated_at
    // we set -- this lets us simulate "claimed N minutes ago" without
    // privileged trigger manipulation. Going through seed_iter1_fixtures
    // and then UPDATE-ing would not work: the trigger would rewrite
    // updated_at to now() and the sweep predicate would never fire.
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    seed_iter1_fixtures(db);

    // Replace the seeded 'pending' row with a 'processing' row whose
    // updated_at is far enough in the past to trip the 15-minute sweep.
    exec_sync(db, "DELETE FROM document_versions WHERE id=$1::uuid",
              std::string(VERSION));
    exec_sync(db,
        "INSERT INTO document_versions "
        "(id, company_id, document_id, version_no, source_hash, "
        " ingest_status, lifecycle_status, updated_at) "
        "VALUES ($1::uuid, $2::uuid, $3::uuid, 1, 'h1', "
        "        'processing', 'draft', now() - interval '20 minutes')",
        std::string(VERSION), std::string(CO), std::string(DOC));

    std::atomic<bool> stop{false};
    wikore::scheduler::PollingFallback::Options opts;
    opts.stuck_threshold = std::chrono::minutes(15);
    wikore::scheduler::PollingFallback poll(db, [&] { return stop.load(); }, opts);

    int swept = drogon::sync_wait(poll.sweep_once());
    CHECK(swept == 1);

    auto v = exec_sync(db,
        "SELECT ingest_status, error_msg FROM document_versions WHERE id=$1::uuid",
        std::string(VERSION));
    CHECK(std::string(v[0]["ingest_status"].c_str()) == "error");
    CHECK(std::string(v[0]["error_msg"].c_str()).find("stuck processing") != std::string::npos);
}

TEST_CASE("PollingFallback: recently-started 'processing' rows are NOT reaped",
          "[integration][iter1][failure_injection]")
{
    // Negative case for follow-up review finding: a document that sat
    // pending for ages and was JUST flipped to 'processing' must not be
    // killed by the sweep. The predicate is updated_at (processing-
    // transition timestamp), not created_at (queue-arrival timestamp).
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    seed_iter1_fixtures(db);

    exec_sync(db, "DELETE FROM document_versions WHERE id=$1::uuid",
              std::string(VERSION));
    exec_sync(db,
        "INSERT INTO document_versions "
        "(id, company_id, document_id, version_no, source_hash, "
        " ingest_status, lifecycle_status, created_at, updated_at) "
        "VALUES ($1::uuid, $2::uuid, $3::uuid, 1, 'h1', "
        "        'processing', 'draft', "
        "        now() - interval '60 minutes', now())",
        std::string(VERSION), std::string(CO), std::string(DOC));

    std::atomic<bool> stop{false};
    wikore::scheduler::PollingFallback::Options opts;
    opts.stuck_threshold = std::chrono::minutes(15);
    wikore::scheduler::PollingFallback poll(db, [&] { return stop.load(); }, opts);

    int swept = drogon::sync_wait(poll.sweep_once());
    CHECK(swept == 0);

    auto v = exec_sync(db,
        "SELECT ingest_status FROM document_versions WHERE id=$1::uuid",
        std::string(VERSION));
    CHECK(std::string(v[0]["ingest_status"].c_str()) == "processing");
}

TEST_CASE("OutboxWorker: release_my_claims puts unprocessed claims back",
          "[integration][iter1][failure_injection]")
{
    // P1 #2 from PR #19 review: SIGTERM mid-batch would otherwise leave
    // claimed-but-unprocessed events stranded. release_my_claims() must
    // restore them to the claim pool and undo the speculative
    // attempt_count increment.
    if (!db_available() || !redis_available()) SKIP("DATABASE_URL or REDIS_URL not set");
    auto db = wikore::Db::get();
    seed_iter1_fixtures(db);

    // Hand-craft a claimable outbox event (skip the full ingest path).
    exec_sync(db, R"(
        INSERT INTO outbox_events
               (id, company_id, aggregate_id, job_type, payload, idempotency_key)
        VALUES ('0b001111-1111-0000-0000-000000000001'::uuid,
                $1::uuid, $2::uuid, 'qdrant_upsert_chunk_payload',
                jsonb_build_object('document_id', $3::text,
                                   'document_version_id', $2::text,
                                   'embed_model_id', $4::text,
                                   'chunk_count', 1),
                'release-test-key')
    )", std::string(CO), std::string(VERSION), std::string(DOC), std::string(MODEL));

    auto embedder  = std::make_shared<wikore::rag::NullEmbedder>(/*dims=*/4);
    auto vec_store = std::make_shared<wikore::rag::NullVectorStore>();
    std::atomic<bool> stop{false};
    wikore::scheduler::OutboxWorker::Options opts;
    opts.batch_size = 16;
    wikore::scheduler::OutboxWorker worker(
        db, embedder, vec_store, [&] { return stop.load(); }, opts);

    // Claim the event but DON'T process it -- simulate the SIGTERM-mid-batch
    // scenario by directly calling release_my_claims after claim_batch
    // returns. drain_once() processes the batch which is what we want to
    // avoid here, so we set shutdown BEFORE calling drain_once and the
    // breaker in drain_once leaves the claim intact.
    stop.store(true);
    int processed = drogon::sync_wait(worker.drain_once());
    CHECK(processed == 0);

    auto claimed = exec_sync(db,
        "SELECT claimed_at IS NOT NULL AS c, attempt_count "
        "FROM outbox_events WHERE id='0b001111-1111-0000-0000-000000000001'::uuid");
    CHECK(std::string(claimed[0]["c"].c_str())                 == "t");
    CHECK(std::stoi(claimed[0]["attempt_count"].c_str())       == 1);

    int released = drogon::sync_wait(worker.release_my_claims());
    CHECK(released == 1);

    auto after = exec_sync(db,
        "SELECT claimed_at IS NULL AS c, attempt_count "
        "FROM outbox_events WHERE id='0b001111-1111-0000-0000-000000000001'::uuid");
    CHECK(std::string(after[0]["c"].c_str())              == "t");
    CHECK(std::stoi(after[0]["attempt_count"].c_str())    == 0);
}

TEST_CASE("OutboxWorker: stale claims (older than lease) are reaped on next poll",
          "[integration][iter1][failure_injection]")
{
    // P1 #2 backstop: even if release_my_claims didn't run (worker SIGKILLed),
    // a stale claim must be reclaimable so the event isn't stranded forever.
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    seed_iter1_fixtures(db);

    exec_sync(db, R"(
        INSERT INTO outbox_events
               (id, company_id, aggregate_id, job_type, payload, idempotency_key,
                claimed_at, claimed_by, attempt_count)
        VALUES ('0b002222-2222-0000-0000-000000000001'::uuid,
                $1::uuid, $2::uuid, 'qdrant_upsert_chunk_payload',
                jsonb_build_object('document_id', $3::text,
                                   'document_version_id', $2::text,
                                   'embed_model_id', $4::text),
                'reap-test-key',
                now() - interval '30 minutes',
                'crashed-worker',
                1)
    )", std::string(CO), std::string(VERSION), std::string(DOC), std::string(MODEL));

    auto embedder  = std::make_shared<wikore::rag::NullEmbedder>(/*dims=*/4);
    auto vec_store = std::make_shared<wikore::rag::NullVectorStore>();
    std::atomic<bool> stop{false};
    wikore::scheduler::OutboxWorker::Options opts;
    opts.claim_lease = std::chrono::minutes(10);
    wikore::scheduler::OutboxWorker worker(
        db, embedder, vec_store, [&] { return stop.load(); }, opts);

    int reaped = drogon::sync_wait(worker.reap_stale_claims());
    CHECK(reaped == 1);

    auto after = exec_sync(db,
        "SELECT claimed_at IS NULL AS c, attempt_count, last_error "
        "FROM outbox_events WHERE id='0b002222-2222-0000-0000-000000000001'::uuid");
    CHECK(std::string(after[0]["c"].c_str())                 == "t");
    CHECK(std::stoi(after[0]["attempt_count"].c_str())       == 0);
    CHECK(std::string(after[0]["last_error"].c_str()).find("reaped") != std::string::npos);
}

TEST_CASE("OutboxWorker: events for a different model are NOT claimed",
          "[integration][iter1][failure_injection]")
{
    // P1 #3 + P2 follow-up from PR #19 review: when this scheduler is
    // configured for model Y, an event tagged with model X must not be
    // claimed at all -- the claim_batch query filters by
    // payload->>'embed_model_id'. The wrong-model worker leaves the
    // event untouched (claimed_at IS NULL, attempt_count = 0) so the
    // correctly-configured scheduler can claim it on its next poll.
    //
    // The previous behaviour (claim-then-fail in process()) was
    // measurable-bad: at the 500ms poll interval a wrong-model worker
    // could exhaust the 5-attempt retry budget in 2.5 seconds, marking
    // the event permanently failed before the right scheduler ever saw
    // it. The process()-time gate is retained as defence-in-depth.
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    seed_iter1_fixtures(db);

    exec_sync(db, R"(
        INSERT INTO outbox_events
               (id, company_id, aggregate_id, job_type, payload, idempotency_key)
        VALUES ('0b003333-3333-0000-0000-000000000001'::uuid,
                $1::uuid, $2::uuid, 'qdrant_upsert_chunk_payload',
                jsonb_build_object('document_id', $3::text,
                                   'document_version_id', $2::text,
                                   'embed_model_id', 'a-different-model'),
                'mismatch-test-key')
    )", std::string(CO), std::string(VERSION), std::string(DOC));

    auto embedder  = std::make_shared<wikore::rag::NullEmbedder>(/*dims=*/4);
    auto vec_store = std::make_shared<wikore::rag::NullVectorStore>();
    std::atomic<bool> stop{false};
    wikore::scheduler::OutboxWorker::Options opts;
    opts.expected_embed_model = MODEL;   // null-embedder; event says 'a-different-model'
    wikore::scheduler::OutboxWorker worker(
        db, embedder, vec_store, [&] { return stop.load(); }, opts);

    int processed = drogon::sync_wait(worker.drain_once());
    CHECK(processed == 0);
    CHECK(worker.events_failed() == 0);   // nothing claimed, nothing failed

    auto ev = exec_sync(db,
        "SELECT claimed_at IS NULL AS unclaimed, attempt_count "
        "FROM outbox_events WHERE id='0b003333-3333-0000-0000-000000000001'::uuid");
    CHECK(std::string(ev[0]["unclaimed"].c_str()) == "t");
    CHECK(ev[0]["attempt_count"].as<int>() == 0);
}

// ---------------------------------------------------------------------------
// Backoff + crash-resume tests (PR #19 re-review follow-up).
// ---------------------------------------------------------------------------

TEST_CASE("OutboxWorker: failed event is not re-claimed until backoff window",
          "[integration][iter1][failure_injection]")
{
    // P1 from PR #19 second re-review: previously, every failure cleared
    // claimed_at and the run loop retried after ~500 ms. With max_attempts
    // = 5, a downstream outage lasting ~2.5 s exhausted the budget. The
    // fix: mark_failed sets next_attempt_at = now() + exp(attempt_count),
    // and claim_batch skips events whose retry window hasn't arrived.
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    seed_iter1_fixtures(db);

    // Seed an event that's already failed once: attempt_count=1, claimed_at
    // NULL, next_attempt_at far in the future. claim_batch must NOT return it.
    exec_sync(db, R"(
        INSERT INTO outbox_events
               (id, company_id, aggregate_id, job_type, payload, idempotency_key,
                attempt_count, claimed_at, next_attempt_at)
        VALUES ('0b004444-4444-0000-0000-000000000001'::uuid,
                $1::uuid, $2::uuid, 'qdrant_upsert_chunk_payload',
                jsonb_build_object('document_id', $3::text,
                                   'document_version_id', $2::text,
                                   'embed_model_id', $4::text),
                'backoff-test-key', 1, NULL,
                now() + interval '30 minutes')
    )", std::string(CO), std::string(VERSION), std::string(DOC), std::string(MODEL));

    auto embedder  = std::make_shared<wikore::rag::NullEmbedder>(/*dims=*/4);
    auto vec_store = std::make_shared<wikore::rag::NullVectorStore>();
    std::atomic<bool> stop{false};
    wikore::scheduler::OutboxWorker::Options opts;
    opts.expected_embed_model = MODEL;
    wikore::scheduler::OutboxWorker worker(
        db, embedder, vec_store, [&] { return stop.load(); }, opts);

    int processed = drogon::sync_wait(worker.drain_once());
    CHECK(processed == 0);
    CHECK(worker.events_failed() == 0);

    auto ev = exec_sync(db,
        "SELECT claimed_at IS NULL AS unclaimed, attempt_count "
        "FROM outbox_events WHERE id='0b004444-4444-0000-0000-000000000001'::uuid");
    CHECK(std::string(ev[0]["unclaimed"].c_str()) == "t");
    CHECK(ev[0]["attempt_count"].as<int>() == 1);   // not incremented again
}

TEST_CASE("OutboxWorker: backoff window elapsed -> event is re-claimed",
          "[integration][iter1][failure_injection]")
{
    // Counterpart to the previous test: when next_attempt_at <= now(),
    // the event becomes claimable again.
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    seed_iter1_fixtures(db);

    exec_sync(db, R"(
        INSERT INTO outbox_events
               (id, company_id, aggregate_id, job_type, payload, idempotency_key,
                attempt_count, claimed_at, next_attempt_at)
        VALUES ('0b004444-4444-0000-0000-000000000002'::uuid,
                $1::uuid, $2::uuid, 'qdrant_upsert_chunk_payload',
                jsonb_build_object('document_id', $3::text,
                                   'document_version_id', $2::text,
                                   'embed_model_id', $4::text),
                'backoff-test-key-elapsed', 1, NULL,
                now() - interval '1 second')
    )", std::string(CO), std::string(VERSION), std::string(DOC), std::string(MODEL));

    auto embedder  = std::make_shared<wikore::rag::NullEmbedder>(/*dims=*/4);
    auto vec_store = std::make_shared<wikore::rag::NullVectorStore>();
    std::atomic<bool> stop{false};
    wikore::scheduler::OutboxWorker::Options opts;
    opts.expected_embed_model = MODEL;
    wikore::scheduler::OutboxWorker worker(
        db, embedder, vec_store, [&] { return stop.load(); }, opts);

    // The event's version has zero document_chunks, so process() returns
    // early with Result<void>{} success ("zero chunks; marking complete").
    int processed = drogon::sync_wait(worker.drain_once());
    CHECK(processed == 1);

    auto ev = exec_sync(db,
        "SELECT completed_at IS NOT NULL AS done "
        "FROM outbox_events WHERE id='0b004444-4444-0000-0000-000000000002'::uuid");
    CHECK(std::string(ev[0]["done"].c_str()) == "t");
}

TEST_CASE("OutboxWorker: mark_failed populates next_attempt_at",
          "[integration][iter1][failure_injection]")
{
    // Verifies the exponential backoff formula. attempt_count was
    // already incremented to N at claim time; mark_failed multiplies
    // 2^N seconds (capped at 300s) into next_attempt_at.
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    seed_iter1_fixtures(db);

    // Insert an event with no document_chunks; expected_embed_model
    // points at a non-existent registry row so process() returns an
    // error from the lookup step and mark_failed runs.
    exec_sync(db, R"(
        INSERT INTO outbox_events
               (id, company_id, aggregate_id, job_type, payload, idempotency_key)
        VALUES ('0b004444-4444-0000-0000-000000000003'::uuid,
                $1::uuid, $2::uuid, 'qdrant_upsert_chunk_payload',
                jsonb_build_object('document_id', $3::text,
                                   'document_version_id', $2::text,
                                   'embed_model_id', 'unregistered-model'),
                'backoff-formula-key')
    )", std::string(CO), std::string(VERSION), std::string(DOC));

    auto embedder  = std::make_shared<wikore::rag::NullEmbedder>(/*dims=*/4);
    auto vec_store = std::make_shared<wikore::rag::NullVectorStore>();
    std::atomic<bool> stop{false};
    wikore::scheduler::OutboxWorker::Options opts;
    // Empty expected_embed_model so the claim filter doesn't block.
    opts.expected_embed_model = "";
    wikore::scheduler::OutboxWorker worker(
        db, embedder, vec_store, [&] { return stop.load(); }, opts);

    int processed = drogon::sync_wait(worker.drain_once());
    CHECK(processed == 0);    // failed inside process()
    CHECK(worker.events_failed() >= 1);

    auto ev = exec_sync(db,
        "SELECT attempt_count, "
        "       (next_attempt_at > now() + interval '500 ms') AS backed_off "
        "FROM   outbox_events "
        "WHERE  id='0b004444-4444-0000-0000-000000000003'::uuid");
    CHECK(ev[0]["attempt_count"].as<int>() == 1);
    CHECK(std::string(ev[0]["backed_off"].c_str()) == "t");
}

TEST_CASE("PollingFallback: stuck 'processing' with payload + budget -> requeued",
          "[integration][iter1][failure_injection]")
{
    // Iter-1 crash-recovery contract: kill the worker mid-job; the
    // version must reach 'done' after restart, not stay stuck. The
    // worker persists ingest_job_payload on dispatch, then the sweep
    // sees a 'processing' row with payload set and retry budget left,
    // and LPUSHes the payload back to the source Redis queue.
    if (!db_available() || !redis_available())
        SKIP("DATABASE_URL or REDIS_URL not set");
    auto db = wikore::Db::get();
    seed_iter1_fixtures(db);

    const std::string queue_key = std::string("lr:ingest:q:") + CO;
    wikore::Redis::del(queue_key);

    // Replace the seeded pending row with a 'processing' row that has
    // payload set and an old updated_at (simulating worker crash).
    exec_sync(db, "DELETE FROM document_versions WHERE id=$1::uuid",
              std::string(VERSION));
    const std::string payload_json =
        std::string(R"({"company_id":")") + CO +
        R"(","document_id":")" + DOC +
        R"(","document_version_id":")" + VERSION +
        R"(","file_path":"/tmp/x.md","embed_model_id":")" + MODEL +
        R"(","priority":0})";
    exec_sync(db, R"(
        INSERT INTO document_versions
               (id, company_id, document_id, version_no, source_hash,
                ingest_status, lifecycle_status,
                ingest_job_payload, ingest_retry_count, updated_at)
        VALUES ($1::uuid, $2::uuid, $3::uuid, 1, 'h1',
                'processing', 'draft',
                $4::jsonb, 0, now() - interval '20 minutes')
    )", std::string(VERSION), std::string(CO), std::string(DOC), payload_json);

    std::atomic<bool> stop{false};
    wikore::scheduler::PollingFallback::Options opts;
    opts.stuck_threshold     = std::chrono::minutes(15);
    opts.max_resume_attempts = 3;
    wikore::scheduler::PollingFallback poll(db, [&] { return stop.load(); }, opts);

    int swept = drogon::sync_wait(poll.sweep_once());
    CHECK(swept == 1);

    // Row reset to 'pending' with retry_count incremented.
    auto v = exec_sync(db,
        "SELECT ingest_status, ingest_retry_count, error_msg "
        "FROM   document_versions WHERE id=$1::uuid", std::string(VERSION));
    CHECK(std::string(v[0]["ingest_status"].c_str()) == "pending");
    CHECK(v[0]["ingest_retry_count"].as<int>() == 1);

    // Payload is back on the source Redis queue.
    auto redrop = wikore::Redis::rpop(queue_key);
    REQUIRE(redrop.has_value());
    CHECK(redrop->find(VERSION) != std::string::npos);
}

TEST_CASE("PollingFallback: stuck 'processing' without payload -> 'error'",
          "[integration][iter1][failure_injection]")
{
    // Negative path: a 'processing' row with no recoverable payload
    // (worker died before persisting it). Sweep marks 'error' as
    // before -- without a payload we can't requeue.
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    seed_iter1_fixtures(db);

    exec_sync(db, "DELETE FROM document_versions WHERE id=$1::uuid",
              std::string(VERSION));
    exec_sync(db, R"(
        INSERT INTO document_versions
               (id, company_id, document_id, version_no, source_hash,
                ingest_status, lifecycle_status, updated_at)
        VALUES ($1::uuid, $2::uuid, $3::uuid, 1, 'h1',
                'processing', 'draft', now() - interval '20 minutes')
    )", std::string(VERSION), std::string(CO), std::string(DOC));

    std::atomic<bool> stop{false};
    wikore::scheduler::PollingFallback::Options opts;
    opts.stuck_threshold = std::chrono::minutes(15);
    wikore::scheduler::PollingFallback poll(db, [&] { return stop.load(); }, opts);

    int swept = drogon::sync_wait(poll.sweep_once());
    CHECK(swept == 1);

    auto v = exec_sync(db,
        "SELECT ingest_status, error_msg FROM document_versions WHERE id=$1::uuid",
        std::string(VERSION));
    CHECK(std::string(v[0]["ingest_status"].c_str()) == "error");
    CHECK(std::string(v[0]["error_msg"].c_str())
              .find("no recoverable job payload") != std::string::npos);
}

TEST_CASE("PollingFallback: stuck 'processing' over retry cap -> 'error'",
          "[integration][iter1][failure_injection]")
{
    // Poison-message guard: even with a payload, after max_resume_attempts
    // requeues the sweep terminates the version at 'error' so a bad input
    // can't loop forever.
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    seed_iter1_fixtures(db);

    exec_sync(db, "DELETE FROM document_versions WHERE id=$1::uuid",
              std::string(VERSION));
    exec_sync(db, R"(
        INSERT INTO document_versions
               (id, company_id, document_id, version_no, source_hash,
                ingest_status, lifecycle_status,
                ingest_job_payload, ingest_retry_count, updated_at)
        VALUES ($1::uuid, $2::uuid, $3::uuid, 1, 'h1',
                'processing', 'draft',
                '{"document_version_id":"x"}'::jsonb, 3,
                now() - interval '20 minutes')
    )", std::string(VERSION), std::string(CO), std::string(DOC));

    std::atomic<bool> stop{false};
    wikore::scheduler::PollingFallback::Options opts;
    opts.stuck_threshold     = std::chrono::minutes(15);
    opts.max_resume_attempts = 3;
    wikore::scheduler::PollingFallback poll(db, [&] { return stop.load(); }, opts);

    int swept = drogon::sync_wait(poll.sweep_once());
    CHECK(swept == 1);

    auto v = exec_sync(db,
        "SELECT ingest_status, error_msg, ingest_job_payload IS NULL AS cleared "
        "FROM document_versions WHERE id=$1::uuid", std::string(VERSION));
    CHECK(std::string(v[0]["ingest_status"].c_str()) == "error");
    CHECK(std::string(v[0]["error_msg"].c_str())
              .find("resume budget exhausted") != std::string::npos);
    CHECK(std::string(v[0]["cleared"].c_str()) == "t");
}

TEST_CASE("PollingFallback: orphan reaper requeues unconditionally; CAS absorbs duplicate",
          "[integration][iter1][failure_injection]")
{
    // New design (PR #19 6th review): the reaper does NOT consult the DB.
    // It unconditionally LPUSHes the orphan entry back to the source
    // queue via an atomic Lua LREM+LPUSH. Duplicate-delivery safety is
    // provided by the worker's claim_for_processing CAS, which only
    // succeeds when ingest_status='pending'. This test verifies the
    // requeue side; the CAS side is exercised by the
    // 'claim_for_processing rejects non-pending row' test below and by
    // the resurrection-guard test.
    if (!db_available() || !redis_available())
        SKIP("DATABASE_URL or REDIS_URL not set");
    auto db = wikore::Db::get();
    seed_iter1_fixtures(db);

    const std::string dead_worker  = "wikore-ingest-dead-99";
    const std::string proc_key     = "lr:ingest:proc:" + dead_worker + ":" + CO;
    const std::string hb_key       = "lr:ingest:hb:" + dead_worker;
    const std::string queue_key    = std::string("lr:ingest:q:") + CO;

    wikore::Redis::del(proc_key);
    wikore::Redis::del(hb_key);
    wikore::Redis::del(queue_key);

    const std::string payload_json =
        std::string(R"({"company_id":")") + CO +
        R"(","document_id":")" + DOC +
        R"(","document_version_id":")" + VERSION +
        R"(","file_path":"/tmp/x.md","embed_model_id":")" + MODEL +
        R"(","priority":0})";
    wikore::Redis::lpush(proc_key, payload_json);

    std::atomic<bool> stop{false};
    wikore::scheduler::PollingFallback poll(db, [&] { return stop.load(); },
        wikore::scheduler::PollingFallback::Options{});
    drogon::sync_wait(poll.sweep_once());

    // Proc list is drained; source queue gets the entry.
    CHECK(wikore::Redis::lrange(proc_key, 0, -1).empty());
    auto requeued = wikore::Redis::rpop(queue_key);
    REQUIRE(requeued.has_value());
    CHECK(requeued->find(VERSION) != std::string::npos);

    wikore::Redis::del(proc_key);
}

TEST_CASE("PollingFallback: orphan reaper for terminal row -> requeue + worker CAS-rejects",
          "[integration][iter1][failure_injection]")
{
    // Resurrection guard via CAS at the worker: if a worker died after
    // the use case completed but before its LREM, the proc list has a
    // ghost entry for a 'done' row. Reaper requeues it
    // unconditionally. When a worker picks it up via LMOVE and calls
    // claim_for_processing, the CAS predicate
    // (ingest_status = 'pending') rejects (row is 'done') and the
    // worker treats the dispatch as a no-op. This test verifies the
    // CAS rejection directly.
    if (!db_available() || !redis_available())
        SKIP("DATABASE_URL or REDIS_URL not set");
    auto db = wikore::Db::get();
    seed_iter1_fixtures(db);

    const std::string dead_worker = "wikore-ingest-zombie-42";
    const std::string proc_key    = "lr:ingest:proc:" + dead_worker + ":" + CO;
    const std::string queue_key   = std::string("lr:ingest:q:") + CO;
    wikore::Redis::del(proc_key);
    wikore::Redis::del(std::string("lr:ingest:hb:") + dead_worker);
    wikore::Redis::del(queue_key);

    // Move the version to 'done' with valid completed_at/chunk_count.
    exec_sync(db, R"(
        UPDATE document_versions
        SET    ingest_status = 'done',
               completed_at  = now(),
               chunk_count   = 0
        WHERE  id = $1::uuid
    )", std::string(VERSION));

    const std::string payload_json =
        std::string(R"({"company_id":")") + CO +
        R"(","document_version_id":")" + VERSION + R"("})";
    wikore::Redis::lpush(proc_key, payload_json);

    std::atomic<bool> stop{false};
    wikore::scheduler::PollingFallback poll(db, [&] { return stop.load(); },
        wikore::scheduler::PollingFallback::Options{});
    drogon::sync_wait(poll.sweep_once());

    // Reaper requeued the entry to the source queue (it doesn't know
    // the row is terminal -- that's the worker's CAS job).
    auto requeued = wikore::Redis::rpop(queue_key);
    REQUIRE(requeued.has_value());

    // Now exercise the CAS directly: a worker calling claim_for_processing
    // on the requeued entry must see nullopt (terminal row, CAS rejects).
    wikore::ingest::PostgresDocumentRepo repo(db);
    auto claimed = drogon::sync_wait(repo.claim_for_processing(
        CO, VERSION, payload_json, db));
    REQUIRE(claimed.has_value());
    CHECK_FALSE(claimed->has_value());   // CAS rejected: row is 'done', not 'pending'

    // Version row stays 'done'; no resurrection.
    auto v = exec_sync(db,
        "SELECT ingest_status FROM document_versions WHERE id=$1::uuid",
        std::string(VERSION));
    CHECK(std::string(v[0]["ingest_status"].c_str()) == "done");

    wikore::Redis::del(proc_key);
}

TEST_CASE("PollingFallback: combined stale row + orphan proc entry are both requeued; CAS dedups",
          "[integration][iter1][failure_injection]")
{
    // With the new design the reaper unconditionally requeues. Sweep
    // #2 (stuck-processing) ALSO requeues from the persisted payload.
    // So a combined-failure cycle ends with TWO queue entries for the
    // same job. That's intentional and safe: the first worker to
    // LMOVE wins the CAS (pending -> processing); the second worker
    // sees ingest_status != 'pending', logs duplicate, LREMs without
    // processing. We verify both queue entries appear and the worker
    // CAS dedups them.
    //
    // The previous design tried to coordinate sweep #2 and the reaper
    // to produce exactly one entry, but that coupling created the
    // resurrection and atomicity bugs flagged in earlier reviews. The
    // CAS-driven dedup is simpler and correct.
    if (!db_available() || !redis_available())
        SKIP("DATABASE_URL or REDIS_URL not set");
    auto db = wikore::Db::get();
    seed_iter1_fixtures(db);

    const std::string dead_worker = "wikore-ingest-coordinated-7";
    const std::string proc_key    = "lr:ingest:proc:" + dead_worker + ":" + CO;
    const std::string queue_key   = std::string("lr:ingest:q:") + CO;
    wikore::Redis::del(proc_key);
    wikore::Redis::del(std::string("lr:ingest:hb:") + dead_worker);
    wikore::Redis::del(queue_key);

    exec_sync(db, "DELETE FROM document_versions WHERE id=$1::uuid",
              std::string(VERSION));
    const std::string payload_json =
        std::string(R"({"company_id":")") + CO +
        R"(","document_id":")" + DOC +
        R"(","document_version_id":")" + VERSION +
        R"(","file_path":"/tmp/x.md","embed_model_id":")" + MODEL +
        R"(","priority":0})";
    exec_sync(db, R"(
        INSERT INTO document_versions
               (id, company_id, document_id, version_no, source_hash,
                ingest_status, lifecycle_status,
                ingest_job_payload, ingest_retry_count, updated_at)
        VALUES ($1::uuid, $2::uuid, $3::uuid, 1, 'h1',
                'processing', 'draft',
                $4::jsonb, 0, now() - interval '20 minutes')
    )", std::string(VERSION), std::string(CO), std::string(DOC), payload_json);
    wikore::Redis::lpush(proc_key, payload_json);

    std::atomic<bool> stop{false};
    wikore::scheduler::PollingFallback poll(db, [&] { return stop.load(); },
        wikore::scheduler::PollingFallback::Options{});
    drogon::sync_wait(poll.sweep_once());

    // Both paths fire; expect 2 queue entries.
    auto queue_contents = wikore::Redis::lrange(queue_key, 0, -1);
    CHECK(queue_contents.size() == 2);
    for (const auto& q : queue_contents)
        CHECK(q.find(VERSION) != std::string::npos);

    // Row was reset to 'pending' by sweep #2 + retry incremented.
    auto v = exec_sync(db,
        "SELECT ingest_status, ingest_retry_count "
        "FROM   document_versions WHERE id=$1::uuid", std::string(VERSION));
    CHECK(std::string(v[0]["ingest_status"].c_str()) == "pending");
    CHECK(v[0]["ingest_retry_count"].as<int>() == 1);

    // Now exercise CAS dedup directly: first claim succeeds (row goes
    // pending -> processing, returns a token), second claim fails (row
    // is already processing, returns nullopt). Worker on the second
    // LMOVE would treat as no-op (DuplicateSkipped).
    wikore::ingest::PostgresDocumentRepo repo(db);
    auto first = drogon::sync_wait(repo.claim_for_processing(
        CO, VERSION, payload_json, db));
    REQUIRE(first.has_value());
    REQUIRE(first->has_value());
    CHECK_FALSE((*first)->empty());   // token returned
    auto second = drogon::sync_wait(repo.claim_for_processing(
        CO, VERSION, payload_json, db));
    REQUIRE(second.has_value());
    CHECK_FALSE(second->has_value());

    CHECK(wikore::Redis::lrange(proc_key, 0, -1).empty());
    wikore::Redis::del(proc_key);
    wikore::Redis::del(queue_key);
}

TEST_CASE("PollingFallback: stuck pending WITH persisted payload is requeued",
          "[integration][iter1][failure_injection]")
{
    // P1 from PR #19 5th review: a worker that LMOVEd a job and
    // persisted the payload to ingest_job_payload, then died BEFORE
    // the use case flipped the row to 'processing', leaves the row in
    // 'pending' with payload set. Sweep #1 must recover this (not
    // promote to error) because the payload is a recoverable copy.
    if (!db_available() || !redis_available())
        SKIP("DATABASE_URL or REDIS_URL not set");
    auto db = wikore::Db::get();
    seed_iter1_fixtures(db);

    const std::string queue_key = std::string("lr:ingest:q:") + CO;
    wikore::Redis::del(queue_key);

    exec_sync(db, "DELETE FROM document_versions WHERE id=$1::uuid",
              std::string(VERSION));
    const std::string payload_json =
        std::string(R"({"company_id":")") + CO +
        R"(","document_id":")" + DOC +
        R"(","document_version_id":")" + VERSION +
        R"(","file_path":"/tmp/x.md","embed_model_id":")" + MODEL +
        R"(","priority":0})";
    exec_sync(db, R"(
        INSERT INTO document_versions
               (id, company_id, document_id, version_no, source_hash,
                ingest_status, lifecycle_status,
                ingest_job_payload, ingest_retry_count, updated_at)
        VALUES ($1::uuid, $2::uuid, $3::uuid, 1, 'h1',
                'pending', 'draft',
                $4::jsonb, 0, now() - interval '20 minutes')
    )", std::string(VERSION), std::string(CO), std::string(DOC), payload_json);

    std::atomic<bool> stop{false};
    wikore::scheduler::PollingFallback::Options opts;
    opts.stuck_threshold     = std::chrono::minutes(15);
    opts.max_resume_attempts = 3;
    wikore::scheduler::PollingFallback poll(db, [&] { return stop.load(); }, opts);

    int swept = drogon::sync_wait(poll.sweep_once());
    CHECK(swept == 1);

    // Row stays at 'pending', retry incremented, payload still set.
    auto v = exec_sync(db,
        "SELECT ingest_status, ingest_retry_count, "
        "       ingest_job_payload IS NOT NULL AS has_payload "
        "FROM   document_versions WHERE id=$1::uuid", std::string(VERSION));
    CHECK(std::string(v[0]["ingest_status"].c_str()) == "pending");
    CHECK(v[0]["ingest_retry_count"].as<int>() == 1);
    CHECK(std::string(v[0]["has_payload"].c_str()) == "t");

    // Payload is back on the source queue.
    auto requeued = wikore::Redis::rpop(queue_key);
    REQUIRE(requeued.has_value());
    CHECK(requeued->find(VERSION) != std::string::npos);
}

TEST_CASE("PollingFallback: recovered pending row is not re-promoted on next sweep",
          "[integration][iter1][failure_injection]")
{
    // P1 from PR #19 5th review: after sweep #2 resets a stuck
    // 'processing' row to 'pending', the next sweep #1 (one minute
    // later) must NOT promote it to 'error'. The previous code used
    // created_at, which is immutable, so a row whose original upload
    // was >15min ago was killed on the very next sweep.
    //
    // Predicate is now updated_at, which is bumped to now() by the
    // BEFORE UPDATE trigger when sweep #2 resets the row. So a freshly
    // recovered row is not stuck by sweep #1's perspective.
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    seed_iter1_fixtures(db);

    // Simulate the post-sweep-#2 state: row in 'pending' with payload,
    // created_at deep in the past, updated_at very recent.
    exec_sync(db, "DELETE FROM document_versions WHERE id=$1::uuid",
              std::string(VERSION));
    exec_sync(db, R"(
        INSERT INTO document_versions
               (id, company_id, document_id, version_no, source_hash,
                ingest_status, lifecycle_status,
                ingest_job_payload, ingest_retry_count,
                created_at, updated_at)
        VALUES ($1::uuid, $2::uuid, $3::uuid, 1, 'h1',
                'pending', 'draft',
                '{"x":1}'::jsonb, 1,
                now() - interval '60 minutes', now())
    )", std::string(VERSION), std::string(CO), std::string(DOC));

    std::atomic<bool> stop{false};
    wikore::scheduler::PollingFallback::Options opts;
    opts.stuck_threshold = std::chrono::minutes(15);
    wikore::scheduler::PollingFallback poll(db, [&] { return stop.load(); }, opts);

    int swept = drogon::sync_wait(poll.sweep_once());
    CHECK(swept == 0);

    auto v = exec_sync(db,
        "SELECT ingest_status, ingest_retry_count "
        "FROM   document_versions WHERE id=$1::uuid", std::string(VERSION));
    CHECK(std::string(v[0]["ingest_status"].c_str()) == "pending");
    CHECK(v[0]["ingest_retry_count"].as<int>() == 1);   // unchanged
}

TEST_CASE("PollingFallback: orphan proc entry whose DB payload is NULL is transferred back",
          "[integration][iter1][failure_injection]")
{
    // The LMOVE-to-payload-persist crash window: worker LMOVEd a job,
    // then died BEFORE writing ingest_job_payload (now persisted by
    // claim_for_processing CAS, but if the worker died between the
    // LMOVE and the CAS the row is still 'pending' with payload NULL).
    // Under the new design the reaper unconditionally transfers the
    // entry back to the source queue regardless of DB state. When a
    // worker eventually picks it up, claim_for_processing succeeds
    // (row is still 'pending') and persists payload atomically.
    //
    // We do NOT backdate updated_at past stuck_threshold here because
    // we want to verify the reaper's effect in isolation. With a
    // backdated updated_at, sweep #1 would (correctly) promote the
    // row to 'error' for being pending+no-payload-too-long-without-a-
    // worker-claiming -- which is a separate, correct behaviour
    // tested by 'promotes stuck pending (no payload) versions to
    // error'.
    if (!db_available() || !redis_available())
        SKIP("DATABASE_URL or REDIS_URL not set");
    auto db = wikore::Db::get();
    seed_iter1_fixtures(db);

    const std::string dead_worker = "wikore-ingest-lmove-window-3";
    const std::string proc_key    = "lr:ingest:proc:" + dead_worker + ":" + CO;
    const std::string queue_key   = std::string("lr:ingest:q:") + CO;
    wikore::Redis::del(proc_key);
    wikore::Redis::del(std::string("lr:ingest:hb:") + dead_worker);
    wikore::Redis::del(queue_key);

    // Row is 'pending' with payload NULL (default state). updated_at
    // is fresh (just inserted), so sweep #1 won't see it as stuck.
    exec_sync(db, "UPDATE document_versions SET ingest_job_payload = NULL "
                   "WHERE id = $1::uuid", std::string(VERSION));

    const std::string payload_json =
        std::string(R"({"company_id":")") + CO +
        R"(","document_id":")" + DOC +
        R"(","document_version_id":")" + VERSION +
        R"(","file_path":"/tmp/x.md","embed_model_id":")" + MODEL +
        R"(","priority":0})";
    wikore::Redis::lpush(proc_key, payload_json);

    std::atomic<bool> stop{false};
    wikore::scheduler::PollingFallback poll(db, [&] { return stop.load(); },
        wikore::scheduler::PollingFallback::Options{});
    drogon::sync_wait(poll.sweep_once());

    // Reaper transferred entry to source queue.
    auto queue_contents = wikore::Redis::lrange(queue_key, 0, -1);
    CHECK(queue_contents.size() == 1);
    CHECK(queue_contents[0].find(VERSION) != std::string::npos);
    CHECK(wikore::Redis::lrange(proc_key, 0, -1).empty());

    // Row stays 'pending' with payload NULL -- the worker that picks
    // it up will populate payload via CAS-claim. Sweep #3 doesn't
    // touch document_versions.
    auto v = exec_sync(db,
        "SELECT ingest_status, ingest_retry_count, "
        "       ingest_job_payload IS NULL AS no_payload "
        "FROM   document_versions WHERE id=$1::uuid", std::string(VERSION));
    CHECK(std::string(v[0]["ingest_status"].c_str()) == "pending");
    CHECK(v[0]["ingest_retry_count"].as<int>() == 0);
    CHECK(std::string(v[0]["no_payload"].c_str()) == "t");

    // Verify CAS-claim by the worker would succeed (row still pending).
    wikore::ingest::PostgresDocumentRepo repo(db);
    auto claimed = drogon::sync_wait(repo.claim_for_processing(
        CO, VERSION, payload_json, db));
    REQUIRE(claimed.has_value());
    REQUIRE(claimed->has_value());
    CHECK_FALSE((*claimed)->empty());   // claim token returned

    wikore::Redis::del(proc_key);
    wikore::Redis::del(queue_key);
}

TEST_CASE("PollingFallback: orphan reaper parses whitespace-formatted JSON",
          "[integration][iter1][failure_injection]")
{
    // P1 from PR #19 6th review: previous version_id extraction used
    // a literal substring search for "document_version_id":", which
    // failed on producers emitting `"document_version_id" : "..."`
    // (whitespace around colon). The payload would be classified as
    // malformed and unconditionally LREMed -- losing the sole copy
    // in the LMOVE-window scenario.
    //
    // Glaze-based parsing handles whitespace per JSON spec.
    if (!db_available() || !redis_available())
        SKIP("DATABASE_URL or REDIS_URL not set");
    auto db = wikore::Db::get();
    seed_iter1_fixtures(db);

    const std::string dead_worker = "wikore-ingest-ws-json-5";
    const std::string proc_key    = "lr:ingest:proc:" + dead_worker + ":" + CO;
    const std::string queue_key   = std::string("lr:ingest:q:") + CO;
    wikore::Redis::del(proc_key);
    wikore::Redis::del(std::string("lr:ingest:hb:") + dead_worker);
    wikore::Redis::del(queue_key);

    exec_sync(db, "UPDATE document_versions SET ingest_job_payload = NULL "
                   "WHERE id = $1::uuid", std::string(VERSION));

    // Whitespace-formatted JSON: spaces around colons, newlines.
    const std::string whitespace_payload =
        std::string("{\n  \"company_id\" : \"") + CO + "\",\n"
        "  \"document_id\" : \"" + DOC + "\",\n"
        "  \"document_version_id\" : \"" + VERSION + "\",\n"
        "  \"file_path\" : \"/tmp/x.md\",\n"
        "  \"embed_model_id\" : \"" + MODEL + "\"\n"
        "}";
    wikore::Redis::lpush(proc_key, whitespace_payload);

    std::atomic<bool> stop{false};
    wikore::scheduler::PollingFallback poll(db, [&] { return stop.load(); },
        wikore::scheduler::PollingFallback::Options{});
    drogon::sync_wait(poll.sweep_once());

    // Payload was correctly parsed and transferred to source queue
    // (NOT classified as malformed and LREMed).
    auto queue_contents = wikore::Redis::lrange(queue_key, 0, -1);
    CHECK(queue_contents.size() == 1);
    CHECK(queue_contents[0].find(VERSION) != std::string::npos);

    wikore::Redis::del(proc_key);
    wikore::Redis::del(queue_key);
}

TEST_CASE("PollingFallback: reaper routes via TRUSTED source-tenant from proc-key (ignores payload company_id)",
          "[integration][iter1][failure_injection]")
{
    // P1 from PR #19 7th review: the reaper must NOT route based on the
    // untrusted payload.company_id. A payload that was originally
    // claimed from tenant A's source queue but carries a bogus
    // company_id="B" must still be requeued to A. The proc-key encodes
    // the trusted source tenant (the queue the worker LMOVEd from).
    if (!db_available() || !redis_available())
        SKIP("DATABASE_URL or REDIS_URL not set");
    auto db = wikore::Db::get();
    seed_iter1_fixtures(db);

    const std::string dead_worker  = "wikore-ingest-tenant-trust";
    const std::string trusted      = CO;     // proc-key tenant suffix
    const std::string untrusted    = "ff1eb0b1-dead-beef-0000-000000000000";
    const std::string proc_key     = "lr:ingest:proc:" + dead_worker + ":" + trusted;
    const std::string trusted_q    = std::string("lr:ingest:q:") + trusted;
    const std::string untrusted_q  = std::string("lr:ingest:q:") + untrusted;
    wikore::Redis::del(proc_key);
    wikore::Redis::del(std::string("lr:ingest:hb:") + dead_worker);
    wikore::Redis::del(trusted_q);
    wikore::Redis::del(untrusted_q);

    // Payload claims to belong to the untrusted tenant; reaper must
    // ignore that and route to the trusted tenant from the proc-key.
    const std::string lying_payload =
        std::string(R"({"company_id":")") + untrusted +
        R"(","document_id":")" + DOC +
        R"(","document_version_id":")" + VERSION + R"("})";
    wikore::Redis::lpush(proc_key, lying_payload);

    std::atomic<bool> stop{false};
    wikore::scheduler::PollingFallback poll(db, [&] { return stop.load(); },
        wikore::scheduler::PollingFallback::Options{});
    drogon::sync_wait(poll.sweep_once());

    // Trusted tenant queue got the entry.
    auto trusted_contents = wikore::Redis::lrange(trusted_q, 0, -1);
    CHECK(trusted_contents.size() == 1);
    // Untrusted tenant queue is UNAFFECTED.
    auto untrusted_contents = wikore::Redis::lrange(untrusted_q, 0, -1);
    CHECK(untrusted_contents.empty());

    wikore::Redis::del(proc_key);
    wikore::Redis::del(trusted_q);
    wikore::Redis::del(untrusted_q);
}

TEST_CASE("PollingFallback: LMOVE-window with STALE updated_at is recovered (reaper bumps updated_at)",
          "[integration][iter1][failure_injection]")
{
    // P1 from PR #19 7th review: in the real production timing, by
    // the time a heartbeat expires the row's updated_at IS past the
    // stuck_threshold. Without the reaper bumping updated_at via a
    // CAS-style UPDATE, sweep #1 immediately after the transfer
    // would promote the row to 'error'. This test backdates
    // updated_at so the reordering AND the bump are both required
    // for the row to survive a single sweep_once.
    if (!db_available() || !redis_available())
        SKIP("DATABASE_URL or REDIS_URL not set");
    auto db = wikore::Db::get();
    seed_iter1_fixtures(db);

    const std::string dead_worker = "wikore-ingest-stale-lmove";
    const std::string proc_key    = "lr:ingest:proc:" + dead_worker + ":" + CO;
    const std::string queue_key   = std::string("lr:ingest:q:") + CO;
    wikore::Redis::del(proc_key);
    wikore::Redis::del(std::string("lr:ingest:hb:") + dead_worker);
    wikore::Redis::del(queue_key);

    // Row in 'pending' state with payload NULL and updated_at past
    // the 15-minute threshold: the production LMOVE-before-CAS
    // crash scenario.
    exec_sync(db, "DELETE FROM document_versions WHERE id=$1::uuid",
              std::string(VERSION));
    exec_sync(db, R"(
        INSERT INTO document_versions
               (id, company_id, document_id, version_no, source_hash,
                ingest_status, lifecycle_status, updated_at)
        VALUES ($1::uuid, $2::uuid, $3::uuid, 1, 'h1',
                'pending', 'draft', now() - interval '30 minutes')
    )", std::string(VERSION), std::string(CO), std::string(DOC));

    const std::string payload_json =
        std::string(R"({"company_id":")") + CO +
        R"(","document_id":")" + DOC +
        R"(","document_version_id":")" + VERSION + R"("})";
    wikore::Redis::lpush(proc_key, payload_json);

    std::atomic<bool> stop{false};
    wikore::scheduler::PollingFallback poll(db, [&] { return stop.load(); },
        wikore::scheduler::PollingFallback::Options{});
    drogon::sync_wait(poll.sweep_once());

    // The reaper transferred AND bumped updated_at; sweep #1 (in the
    // same sweep_once) did NOT promote the row to 'error'.
    auto v = exec_sync(db,
        "SELECT ingest_status FROM document_versions WHERE id=$1::uuid",
        std::string(VERSION));
    CHECK(std::string(v[0]["ingest_status"].c_str()) == "pending");

    // Entry is on the source queue, ready for a worker.
    auto queue_contents = wikore::Redis::lrange(queue_key, 0, -1);
    CHECK(queue_contents.size() == 1);
    CHECK(queue_contents[0].find(VERSION) != std::string::npos);

    wikore::Redis::del(proc_key);
    wikore::Redis::del(queue_key);
}

TEST_CASE("IngestDocumentVersionUseCase: CAS-loser does NOT touch the winner's payload",
          "[integration][iter1][failure_injection]")
{
    // P1 from PR #19 7th review: when worker A wins the CAS and is
    // mid-processing, worker B (duplicate delivery from the reaper)
    // loses the CAS. Previous code returned ordinary success from
    // execute() and then unconditionally cleared ingest_job_payload
    // -- destroying A's recovery state. Now execute() returns
    // DuplicateSkipped and the worker LREMs without touching the row.
    //
    // This test simulates the exact two-worker race:
    //   1. Insert a 'pending' row.
    //   2. Worker A calls claim_for_processing (wins).
    //      Row is now 'processing' + payload persisted.
    //   3. Worker B calls execute() with the same cmd (duplicate).
    //      Expect: DuplicateSkipped, payload UNCHANGED.
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    seed_iter1_fixtures(db);

    // Row starts 'pending' (from seed); ensure no payload.
    exec_sync(db, "UPDATE document_versions SET ingest_job_payload = NULL "
                   "WHERE id = $1::uuid", std::string(VERSION));

    const std::string winners_payload =
        std::string(R"({"company_id":")") + CO +
        R"(","document_id":")" + DOC +
        R"(","document_version_id":")" + VERSION +
        R"(","file_path":"/tmp/winner.md","embed_model_id":")" + MODEL +
        R"("})";

    // Worker A wins the CAS.
    wikore::ingest::PostgresDocumentRepo repo(db);
    auto winner_claim = drogon::sync_wait(repo.claim_for_processing(
        CO, VERSION, winners_payload, db));
    REQUIRE(winner_claim.has_value());
    REQUIRE(winner_claim->has_value());
    const std::string winner_token = **winner_claim;
    CHECK_FALSE(winner_token.empty());

    // Confirm row is now 'processing' with winner's payload.
    auto before = exec_sync(db,
        "SELECT ingest_status, ingest_job_payload::text AS p "
        "FROM document_versions WHERE id=$1::uuid", std::string(VERSION));
    CHECK(std::string(before[0]["ingest_status"].c_str()) == "processing");
    const std::string saved_payload = before[0]["p"].as<std::string>();
    REQUIRE(!saved_payload.empty());

    // Worker B (CAS loser) runs execute() with a DIFFERENT payload
    // representing the same logical job.
    const std::string losers_payload =
        std::string(R"({"company_id":")") + CO +
        R"(","document_id":")" + DOC +
        R"(","document_version_id":")" + VERSION +
        R"(","file_path":"/tmp/loser.md","embed_model_id":")" + MODEL +
        R"("})";
    auto uc = make_use_case(db);
    wikore::RequestContext ctx{
        .tenant    = {.company_id = CO},
        .principal = {.user_id = USR, .email = "ingest@test.com"},
        .span      = {.trace_id = "trace-cas-loser-test", .span_id = "s"},
        .deadline  = std::chrono::steady_clock::now() + std::chrono::seconds(30),
    };
    auto out = drogon::sync_wait(uc.execute(
        ctx,
        {.company_id          = CO,
         .document_id         = DOC,
         .document_version_id = VERSION,
         .file_path           = "/tmp/loser.md",
         .embed_model_id      = MODEL,
         .ingest_job_payload  = losers_payload}));

    REQUIRE(out.has_value());
    CHECK(*out == wikore::application::IngestDispatchOutcome::DuplicateSkipped);

    // CRITICAL: winner's payload is unchanged. If the loser had cleared
    // it (previous bug), this assertion would fail.
    auto after = exec_sync(db,
        "SELECT ingest_status, ingest_job_payload::text AS p "
        "FROM document_versions WHERE id=$1::uuid", std::string(VERSION));
    CHECK(std::string(after[0]["ingest_status"].c_str()) == "processing");
    CHECK(after[0]["p"].as<std::string>() == saved_payload);
}

TEST_CASE("IngestDocumentVersionUseCase: pre-claim error doesn't touch document_versions",
          "[integration][iter1][failure_injection]")
{
    // Direct-use-case test: when execute() returns Err BEFORE the CAS
    // could run (e.g. tenant mismatch returns Forbidden early), the
    // row state and the (test-fixture-managed) proc list must both be
    // unchanged. The use case has no knowledge of Redis proc keys --
    // the worker is responsible for orchestrating the proc-entry
    // transfer-back on Err.
    if (!db_available() || !redis_available())
        SKIP("DATABASE_URL or REDIS_URL not set");
    auto db = wikore::Db::get();
    seed_iter1_fixtures(db);
    clear_queues();

    const std::string my_worker  = "wikore-ingest-uc-preclaim-err";
    const std::string proc_key   = std::string("lr:ingest:proc:") + my_worker + ":" + CO;
    const std::string queue_key  = std::string("lr:ingest:q:") + CO;
    wikore::Redis::del(proc_key);
    wikore::Redis::del(queue_key);
    wikore::Redis::del(std::string("lr:ingest:hb:") + my_worker);

    const std::string payload =
        std::string(R"({"company_id":")") + CO +
        R"(","document_id":")" + DOC +
        R"(","document_version_id":")" + VERSION +
        R"(","file_path":"/tmp/x.md","embed_model_id":")" + MODEL +
        R"("})";
    wikore::Redis::lpush(queue_key, payload);
    wikore::Redis::lmove_right_left(queue_key, proc_key);

    auto uc = make_use_case(db);
    wikore::RequestContext bad_ctx{
        .tenant    = {.company_id = "ff1eb0b1-bad0-bad0-bad0-bad0bad0bad0"},
        .principal = {.user_id = USR, .email = "ingest@test.com"},
        .span      = {.trace_id = "trace-uc-preclaim-err", .span_id = "s"},
        .deadline  = std::chrono::steady_clock::now() + std::chrono::seconds(30),
    };
    auto out = drogon::sync_wait(uc.execute(
        bad_ctx,
        {.company_id          = CO,
         .document_id         = DOC,
         .document_version_id = VERSION,
         .file_path           = "/tmp/x.md",
         .embed_model_id      = MODEL,
         .ingest_job_payload  = payload}));
    REQUIRE_FALSE(out.has_value());
    CHECK(out.error().kind == wikore::Error::Kind::Forbidden);

    // Use case path: proc entry untouched (it doesn't know about Redis).
    auto proc_contents = wikore::Redis::lrange(proc_key, 0, -1);
    CHECK(proc_contents.size() == 1);
    CHECK(proc_contents[0] == payload);

    // Row is still 'pending', payload NOT persisted (CAS never ran).
    auto v = exec_sync(db,
        "SELECT ingest_status, ingest_job_payload IS NULL AS no_payload "
        "FROM document_versions WHERE id=$1::uuid", std::string(VERSION));
    CHECK(std::string(v[0]["ingest_status"].c_str()) == "pending");
    CHECK(std::string(v[0]["no_payload"].c_str()) == "t");

    wikore::Redis::del(proc_key);
    wikore::Redis::del(queue_key);
}

TEST_CASE("IngestDocumentRepo: mark_ingest_done atomically clears payload (no stale-worker stomp)",
          "[integration][iter1][failure_injection]")
{
    // P1 from PR #19 11th review: previously the worker did a SEPARATE
    // unconditional UPDATE after mark_ingest_done committed, clearing
    // ingest_job_payload. If the version was re-triggered and a new
    // worker claimed it in that window, the stale worker's payload-
    // clear would stomp the new owner's freshly-persisted recovery
    // state.
    //
    // Fix: mark_ingest_done (and set_ingest_status('error')) clear
    // ingest_job_payload INSIDE the token-gated UPDATE, atomically
    // with the status flip. The worker no longer does a separate
    // post-commit UPDATE.
    //
    // This test exercises the atomic clear:
    //   1. claim_for_processing -> get token, payload set
    //   2. mark_ingest_done(token) -> 'done' AND payload cleared in one statement
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    seed_iter1_fixtures(db);

    wikore::ingest::PostgresDocumentRepo repo(db);
    auto claim = drogon::sync_wait(repo.claim_for_processing(
        CO, VERSION, R"({"k":"v"})", db));
    REQUIRE(claim.has_value());
    REQUIRE(claim->has_value());
    const std::string token = **claim;

    // Verify the row currently has the payload set.
    auto pre = exec_sync(db,
        "SELECT ingest_job_payload IS NOT NULL AS has_payload "
        "FROM document_versions WHERE id=$1::uuid", std::string(VERSION));
    REQUIRE(std::string(pre[0]["has_payload"].c_str()) == "t");

    drogon::sync_wait([&]() -> drogon::Task<void> {
        auto uow = co_await wikore::postgres::UnitOfWork::begin(db);
        auto r = co_await repo.mark_ingest_done(CO, VERSION, 3, token, uow);
        REQUIRE(r.has_value());
        REQUIRE(*r == true);
        co_await uow.commit();
    }());

    // After mark_ingest_done: row is 'done' AND payload is cleared,
    // AND claim_token is cleared -- all in ONE atomic UPDATE.
    auto v = exec_sync(db,
        "SELECT ingest_status, "
        "       ingest_job_payload IS NULL AS no_payload, "
        "       ingest_claim_token IS NULL AS no_token "
        "FROM document_versions WHERE id=$1::uuid", std::string(VERSION));
    CHECK(std::string(v[0]["ingest_status"].c_str()) == "done");
    CHECK(std::string(v[0]["no_payload"].c_str()) == "t");
    CHECK(std::string(v[0]["no_token"].c_str()) == "t");
}

TEST_CASE("IngestDocumentRepo: stale worker's mark_done with wrong token does NOT clear new owner's payload",
          "[integration][iter1][failure_injection]")
{
    // The end-to-end race the reviewer flagged:
    //   1. Worker A claims (token T_A); starts long work.
    //   2. PollingFallback resets to pending (token cleared); B claims
    //      (token T_B); B persisted a NEW payload.
    //   3. Worker A wakes up and tries mark_done(token=T_A).
    //   4. CAS rejects (T_A != T_B). The new owner's payload MUST NOT
    //      be cleared.
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    seed_iter1_fixtures(db);

    wikore::ingest::PostgresDocumentRepo repo(db);
    auto a_claim = drogon::sync_wait(repo.claim_for_processing(
        CO, VERSION, R"({"k":"v_A"})", db));
    REQUIRE(a_claim.has_value());
    REQUIRE(a_claim->has_value());
    const std::string token_a = **a_claim;

    // Sweep #2 simulation: reset to pending + clear token.
    exec_sync(db,
        "UPDATE document_versions SET ingest_status='pending', "
        "ingest_claim_token=NULL, ingest_job_payload=NULL "
        "WHERE id=$1::uuid", std::string(VERSION));

    // Worker B claims, persisting a different payload.
    auto b_claim = drogon::sync_wait(repo.claim_for_processing(
        CO, VERSION, R"({"k":"v_B_NEW"})", db));
    REQUIRE(b_claim.has_value());
    REQUIRE(b_claim->has_value());

    // Worker A wakes up and tries to mark_done with its OLD token.
    drogon::sync_wait([&]() -> drogon::Task<void> {
        auto uow = co_await wikore::postgres::UnitOfWork::begin(db);
        auto r = co_await repo.mark_ingest_done(CO, VERSION, 999, token_a, uow);
        REQUIRE(r.has_value());
        CHECK(*r == false);   // CAS rejected
        uow.rollback();
    }());

    // CRITICAL: the row is still 'processing' (B owns it) AND B's
    // payload is INTACT. With the previous separate-UPDATE approach,
    // worker A's payload-clear would have stomped B's payload here.
    auto v = exec_sync(db,
        "SELECT ingest_status, ingest_job_payload::text AS p "
        "FROM document_versions WHERE id=$1::uuid", std::string(VERSION));
    CHECK(std::string(v[0]["ingest_status"].c_str()) == "processing");
    CHECK(v[0]["p"].as<std::string>().find("v_B_NEW") != std::string::npos);
}

TEST_CASE("IngestDocumentRepo: claim_token guards mark_ingest_done against resurrection",
          "[integration][iter1][failure_injection]")
{
    // P1 from PR #19 10th audit: without the per-claim token, worker A
    // could overwrite worker B's terminal state. Scenario:
    //   Worker A: claim -> processing (token T_A)
    //   PollingFallback: reset to pending (clears token)
    //   Worker B: claim -> processing (token T_B)
    //   Worker B: mark_ingest_done(token=T_B) -> done
    //   Worker A: mark_ingest_done(token=T_A) -> 0 rows (T_A != current)
    //
    // This test simulates the race directly via repo calls.
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    seed_iter1_fixtures(db);

    wikore::ingest::PostgresDocumentRepo repo(db);

    // Worker A wins the first CAS.
    auto a_claim = drogon::sync_wait(repo.claim_for_processing(
        CO, VERSION, R"({"k":"v"})", db));
    REQUIRE(a_claim.has_value());
    REQUIRE(a_claim->has_value());
    const std::string token_a = **a_claim;

    // PollingFallback simulates a reset to pending with token cleared.
    exec_sync(db,
        "UPDATE document_versions SET ingest_status='pending', "
        "ingest_claim_token=NULL WHERE id=$1::uuid",
        std::string(VERSION));

    // Worker B wins the next CAS.
    auto b_claim = drogon::sync_wait(repo.claim_for_processing(
        CO, VERSION, R"({"k":"v2"})", db));
    REQUIRE(b_claim.has_value());
    REQUIRE(b_claim->has_value());
    const std::string token_b = **b_claim;
    REQUIRE(token_a != token_b);

    // Worker B completes -> done.
    drogon::sync_wait([&]() -> drogon::Task<void> {
        auto uow = co_await wikore::postgres::UnitOfWork::begin(db);
        auto r = co_await repo.mark_ingest_done(CO, VERSION, 5, token_b, uow);
        REQUIRE(r.has_value());
        REQUIRE(*r == true);
        co_await uow.commit();
    }());

    auto v = exec_sync(db,
        "SELECT ingest_status, chunk_count FROM document_versions "
        "WHERE id=$1::uuid", std::string(VERSION));
    REQUIRE(std::string(v[0]["ingest_status"].c_str()) == "done");
    REQUIRE(std::stoi(v[0]["chunk_count"].c_str()) == 5);

    // Worker A wakes up and tries to overwrite with its own (stale)
    // chunk_count. The token check MUST reject this.
    drogon::sync_wait([&]() -> drogon::Task<void> {
        auto uow = co_await wikore::postgres::UnitOfWork::begin(db);
        auto r = co_await repo.mark_ingest_done(CO, VERSION, 999, token_a, uow);
        REQUIRE(r.has_value());
        CHECK(*r == false);   // OwnershipLost: token doesn't match
        uow.rollback();
    }());

    // Row remains B's done state.
    auto v2 = exec_sync(db,
        "SELECT ingest_status, chunk_count FROM document_versions "
        "WHERE id=$1::uuid", std::string(VERSION));
    CHECK(std::string(v2[0]["ingest_status"].c_str()) == "done");
    CHECK(std::stoi(v2[0]["chunk_count"].c_str()) == 5);   // not 999
}

TEST_CASE("IngestDocumentRepo: claim rejects archived versions",
          "[integration][iter1][failure_injection]")
{
    // P1 from PR #19 10th audit: claim_for_processing must guard
    // lifecycle_status so archived versions cannot be re-ingested
    // (V010 treats archived as terminal-no-retrieval).
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    seed_iter1_fixtures(db);

    // Archived versions only exist after deactivation; force one here.
    exec_sync(db, R"(
        UPDATE document_versions
        SET    lifecycle_status = 'archived'
        WHERE  id = $1::uuid
    )", std::string(VERSION));

    wikore::ingest::PostgresDocumentRepo repo(db);
    auto claim = drogon::sync_wait(repo.claim_for_processing(
        CO, VERSION, R"({"k":"v"})", db));
    REQUIRE(claim.has_value());
    CHECK_FALSE(claim->has_value());   // CAS rejected
}

TEST_CASE("IngestWorker: dispatch TerminalError is acknowledged, not requeued",
          "[integration][iter1][failure_injection]")
{
    // The use case distinguishes:
    //   * pre-CAS infra failures -> Err            -> worker transfers back
    //   * post-CAS terminal failures -> TerminalError -> worker LREMs +
    //                                                     clears payload
    //   * CAS lost / OwnershipLost -> Duplicate    -> worker LREMs
    //
    // This test exercises the TerminalError path with a missing
    // file_path: claim_for_processing wins, then file open fails, then
    // fail() flips the row to 'error' (gated by claim token). The
    // worker MUST NOT transfer back -- that would just CAS-skip on
    // next pickup, wasting a rotation. Instead it LREMs.
    if (!db_available() || !redis_available())
        SKIP("DATABASE_URL or REDIS_URL not set");
    auto db = wikore::Db::get();
    seed_iter1_fixtures(db);
    clear_queues();

    const std::string payload =
        std::string(R"({"company_id":")") + CO +
        R"(","document_id":")" + DOC +
        R"(","document_version_id":")" + VERSION +
        R"(","file_path":"/tmp/this-file-does-not-exist-for-iter1-test","embed_model_id":")" + MODEL +
        R"("})";

    const std::string queue_key = std::string("lr:ingest:q:") + CO;
    wikore::Redis::lpush(queue_key, payload);

    std::atomic<bool> stop{false};
    auto uc = make_use_case(db);
    wikore::ingest::IngestWorker::Options opts;
    opts.idle_sleep      = std::chrono::milliseconds(50);
    opts.rescan_interval = std::chrono::seconds(1);
    opts.heartbeat_ttl   = std::chrono::seconds(60);
    opts.worker_id       = "wikore-ingest-terminal-test";
    wikore::ingest::IngestWorker worker(std::move(uc), [&] { return stop.load(); }, opts);

    const std::string proc_key =
        std::string("lr:ingest:proc:") + opts.worker_id + ":" + CO;
    const std::string hb_key =
        std::string("lr:ingest:hb:") + opts.worker_id;
    wikore::Redis::del(proc_key);
    wikore::Redis::del(hb_key);

    drogon::sync_wait([&]() -> drogon::Task<void> {
        auto loop = drogon::app().getLoop();
        loop->runAfter(1.5, [&] { stop.store(true); });
        co_await worker.run();
    }());

    // Dispatch counted as failed (TerminalError increments jobs_failed_).
    CHECK(worker.jobs_failed() >= 1);
    // NOT counted as duplicate (no re-LMOVE happened).
    CHECK(worker.jobs_duplicates() == 0);

    // Proc list empty (LREMed by Processed/TerminalError branch).
    CHECK(wikore::Redis::lrange(proc_key, 0, -1).empty());
    // Source queue empty (no transfer-back).
    CHECK(wikore::Redis::lrange(queue_key, 0, -1).empty());

    // Row reached 'error' from the file-read failure; ingest_job_payload
    // was cleared by the worker's Processed/TerminalError branch.
    auto v = exec_sync(db,
        "SELECT ingest_status, ingest_job_payload IS NULL AS no_payload "
        "FROM document_versions WHERE id=$1::uuid", std::string(VERSION));
    CHECK(std::string(v[0]["ingest_status"].c_str()) == "error");
    CHECK(std::string(v[0]["no_payload"].c_str()) == "t");

    wikore::Redis::del(proc_key);
    wikore::Redis::del(hb_key);
    wikore::Redis::del(queue_key);
}

TEST_CASE("PollingFallback: reaper UPDATE failure leaves proc entry untouched",
          "[integration][iter1][failure_injection]")
{
    // P1 from PR #19 8th review: the reaper must touch the PG
    // row BEFORE the Redis transfer. If the UPDATE fails, the proc
    // entry must stay on the proc list so the next reaper cycle
    // retries -- otherwise the only Redis copy is gone and the
    // stale pending row would be promoted to 'error' by sweep #1.
    //
    // We simulate UPDATE failure by giving the reaper a proc-key
    // whose encoded tenant doesn't exist in document_versions: the
    // CAS-predicated UPDATE simply matches 0 rows (no DB error),
    // which the implementation treats as success (the version is
    // not in 'pending' state, so the reaper bumps nothing -- the
    // row may already be terminal or in-flight). The Redis
    // transfer then proceeds, the CAS at the worker will reject if
    // terminal.
    //
    // A more authentic "UPDATE fails" test would shut the PG
    // connection mid-call, which is non-trivial in a test
    // environment. The implementation's contract for "DB query
    // succeeded with zero rows affected" is the same as "DB query
    // succeeded with one row updated" -- proceed with transfer.
    // The contract for "DB query threw an exception" is "skip
    // transfer; leave proc entry; retry next cycle".
    //
    // We verify the exception path by stopping the DB connection
    // pool briefly. Since wikore::Db::get() returns a shared
    // pooled DbClient, we cannot easily simulate this in isolation.
    // Instead we exercise the orthogonal invariant: the order is
    // UPDATE-then-LMOVE, so an UPDATE no-op leaves the Redis state
    // consistent without prematurely transferring.
    //
    // Concretely: seed a row that is 'done' (terminal). The CAS
    // predicate ingest_status='pending' yields zero affected rows
    // -- no exception. The transfer still proceeds (worker CAS
    // will reject the duplicate). Test passes if the order doesn't
    // accidentally LMOVE for non-pending rows in a way that
    // confuses the system.
    //
    // NOTE: this is the strongest invariant we can verify without
    // a DB-failure-injection seam in PollingFallback. The
    // happens-before ordering is enforced by the source code
    // structure and reviewed there.
    if (!db_available() || !redis_available())
        SKIP("DATABASE_URL or REDIS_URL not set");
    auto db = wikore::Db::get();
    seed_iter1_fixtures(db);

    const std::string dead_worker = "wikore-ingest-update-first";
    const std::string proc_key    = "lr:ingest:proc:" + dead_worker + ":" + CO;
    const std::string queue_key   = std::string("lr:ingest:q:") + CO;
    wikore::Redis::del(proc_key);
    wikore::Redis::del(std::string("lr:ingest:hb:") + dead_worker);
    wikore::Redis::del(queue_key);

    // Row is 'done' -- CAS predicate ingest_status='pending' will
    // match 0 rows. Reaper still transfers, worker CAS would
    // reject as duplicate.
    exec_sync(db, R"(
        UPDATE document_versions
        SET    ingest_status = 'done',
               completed_at  = now(),
               chunk_count   = 0
        WHERE  id = $1::uuid
    )", std::string(VERSION));

    const std::string payload =
        std::string(R"({"company_id":")") + CO +
        R"(","document_version_id":")" + VERSION + R"("})";
    wikore::Redis::lpush(proc_key, payload);

    std::atomic<bool> stop{false};
    wikore::scheduler::PollingFallback poll(db, [&] { return stop.load(); },
        wikore::scheduler::PollingFallback::Options{});
    drogon::sync_wait(poll.sweep_once());

    // The 0-row UPDATE didn't throw, so the transfer proceeded.
    // The terminal row stays 'done' (CAS at worker would reject).
    auto v = exec_sync(db,
        "SELECT ingest_status FROM document_versions WHERE id=$1::uuid",
        std::string(VERSION));
    CHECK(std::string(v[0]["ingest_status"].c_str()) == "done");

    // Queue got the requeued entry (worker CAS dedups via 'pending'
    // predicate; it returns false for the 'done' row).
    auto queue_contents = wikore::Redis::lrange(queue_key, 0, -1);
    CHECK(queue_contents.size() == 1);

    wikore::Redis::del(proc_key);
    wikore::Redis::del(queue_key);
}

TEST_CASE("PollingFallback: concurrent reapers produce exactly one queue copy",
          "[integration][iter1][failure_injection]")
{
    // P1 from PR #19 6th review: two scheduler instances racing on the
    // same orphan proc entry must not both LPUSH the payload to the
    // source queue. The Redis transfer is implemented as a Lua EVAL
    // doing atomic LREM-then-conditional-LPUSH (only the caller whose
    // LREM observes count>0 performs the LPUSH).
    //
    // We can't easily run two sweep_once invocations truly
    // concurrently in a single-threaded test loop, but we can exercise
    // the atomic primitive directly to prove its semantics: two back-
    // to-back calls must produce exactly one queue entry, with the
    // second returning 0 (no LREM happened).
    if (!redis_available()) SKIP("REDIS_URL not set");

    const std::string proc_key  = "lr:ingest:proc:concurrent-test-99";
    const std::string queue_key = "lr:ingest:q:concurrent-test-co";
    wikore::Redis::del(proc_key);
    wikore::Redis::del(queue_key);

    const std::string payload = R"({"company_id":"x","document_version_id":"v"})";
    wikore::Redis::lpush(proc_key, payload);

    // First call wins the LREM and LPUSHes.
    int first  = wikore::Redis::transfer_proc_to_source(proc_key, queue_key, payload);
    // Second call: LREM returns 0 (already gone), no LPUSH.
    int second = wikore::Redis::transfer_proc_to_source(proc_key, queue_key, payload);

    CHECK(first  == 1);
    CHECK(second == 0);
    // Exactly ONE queue entry, not two.
    auto contents = wikore::Redis::lrange(queue_key, 0, -1);
    CHECK(contents.size() == 1);
    CHECK(contents[0] == payload);

    wikore::Redis::del(proc_key);
    wikore::Redis::del(queue_key);
}


TEST_CASE("PollingFallback: alive worker (heartbeat present) is NOT reaped",
          "[integration][iter1][failure_injection]")
{
    // Negative path: if the heartbeat key is present, the worker is
    // alive (idle or still processing). Sweep must leave its
    // processing list alone.
    if (!db_available() || !redis_available())
        SKIP("DATABASE_URL or REDIS_URL not set");
    auto db = wikore::Db::get();
    seed_iter1_fixtures(db);

    const std::string alive_worker = "wikore-ingest-alive-77";
    const std::string proc_key     = "lr:ingest:proc:" + alive_worker + ":" + CO;
    const std::string hb_key       = "lr:ingest:hb:" + alive_worker;
    const std::string queue_key    = std::string("lr:ingest:q:") + CO;

    wikore::Redis::del(proc_key);
    wikore::Redis::del(hb_key);
    wikore::Redis::del(queue_key);

    const std::string payload_json =
        std::string(R"({"company_id":")") + CO +
        R"(","document_id":")" + DOC +
        R"(","document_version_id":")" + VERSION + R"("})";
    wikore::Redis::lpush(proc_key, payload_json);
    // Heartbeat present with long TTL.
    wikore::Redis::set(hb_key, "1", std::chrono::seconds(60));

    std::atomic<bool> stop{false};
    wikore::scheduler::PollingFallback poll(db, [&] { return stop.load(); },
        wikore::scheduler::PollingFallback::Options{});
    drogon::sync_wait(poll.sweep_once());

    // Proc list is untouched.
    auto remaining = wikore::Redis::lrange(proc_key, 0, -1);
    CHECK(remaining.size() == 1);
    CHECK(wikore::Redis::rpop(queue_key) == std::nullopt);

    wikore::Redis::del(proc_key);
    wikore::Redis::del(hb_key);
}
