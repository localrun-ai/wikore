#include "wikore/config.hpp"
#include "wikore/db.hpp"
#include "wikore/ingest/document_repo.hpp"
#include "wikore/rag/embedder.hpp"
#include "wikore/rag/vector_store.hpp"
#include "wikore/redis.hpp"
#include "wikore/scheduler/edge_vector_cleanup_worker.hpp"
#include "wikore/scheduler/embed_edge_worker.hpp"
#include "wikore/scheduler/outbox_worker.hpp"
#include "wikore/scheduler/partition_maintainer.hpp"
#include "wikore/scheduler/polling_fallback.hpp"
#include "wikore/scheduler/resync_worker.hpp"
#include <drogon/drogon.h>
#include <spdlog/spdlog.h>
#include <atomic>
#include <cstdlib>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>

// ---------------------------------------------------------------------------
// wikore-scheduler: outbox drainer + resync worker + polling fallback +
// partition maintainer.
//
// Four long-running coroutines on the event loop:
//   * OutboxWorker drains qdrant_upsert_chunk_payload events from V015's
//     outbox_events table (FOR UPDATE SKIP LOCKED), embeds each chunk
//     batch via llama-server, upserts to Qdrant, and writes document_chunk_vectors.
//   * ResyncWorker drains qdrant_resync_chunk_acl events (enqueued by V032's
//     ACL-change triggers), recomputes the resource-axis scope live from
//     Postgres, and rewrites the Qdrant payload's ACL keys WITHOUT re-embedding.
//     It shares OutboxWorker's per-document advisory lock so ingest and resync
//     of a document serialize, and patches every collection the document has
//     points in (see the VectorStoreForCollection resolver below).
//   * PollingFallback advisory-locks per-call, finds document_versions
//     rows stuck in 'pending' or 'processing' beyond 15 minutes, and
//     promotes them to 'error' (or requeues, per V029).
//   * PartitionMaintainer runs daily, pre-creates quarterly audit_log and
//     monthly usage_events partitions 1 year ahead, and alerts on overflow
//     into the catch-all DEFAULT partitions.
//
// SIGTERM/SIGINT are routed through drogon's signal-handler API (NOT
// std::signal, which drogon overwrites in run()). Each handler flips
// g_shutdown; quit() is called only once BOTH workers have returned, so
// outbox claims are released and the polling lock is dropped before exit.
//
// Fail-fast startup paths (missing/disabled embedding model, dimension
// mismatch, unreachable Qdrant) set g_fatal_exit BEFORE app().quit() so
// main() can return EXIT_FAILURE. Without that, a transient infra outage
// at boot would leave the scheduler "successfully exited" and supervisors
// using Restart=on-failure would never restart it -- outbox events would
// pile up and the polling recovery would be permanently offline.
// ---------------------------------------------------------------------------

namespace {
std::atomic<bool> g_shutdown{false};
std::atomic<bool> g_fatal_exit{false};

// All workers must finish their drain path before drogon exits.
std::atomic<int>  g_workers_remaining{6};

void on_worker_exit(std::string_view name)
{
    spdlog::info("[wikore-scheduler] {} exited", name);
    if (g_workers_remaining.fetch_sub(1) == 1) {
        spdlog::info("[wikore-scheduler] all workers drained; quitting drogon");
        drogon::app().quit();
    }
}

// Mark a fatal startup failure: log critical, flip g_fatal_exit so main()
// returns EXIT_FAILURE, and quit drogon's event loop so app().run() returns.
void fail_startup(std::string_view reason)
{
    spdlog::critical("[wikore-scheduler] startup aborted: {}", reason);
    g_fatal_exit.store(true);
    drogon::app().quit();
}
} // namespace

int main()
{
    const auto cfg = wikore::Config::from_env();

    if (cfg.partition_database_url.empty()) {
        spdlog::critical(
            "[wikore-scheduler] PARTITION_DATABASE_URL is required for the "
            "restricted partition-maintenance connection");
        return EXIT_FAILURE;
    }

    spdlog::info("[wikore-scheduler] version: " WIKORE_GIT_HASH);
    spdlog::info("[wikore-scheduler] db:      {}", cfg.database_url);
    spdlog::info("[wikore-scheduler] redis:   {}", cfg.redis_url);
    spdlog::info("[wikore-scheduler] qdrant:  {}", cfg.qdrant_url);
    spdlog::info("[wikore-scheduler] embed:   {}", cfg.embed_base_url);

    // Pool sized for the concurrent worker connection demand: ResyncWorker
    // holds a transaction (advisory lock) AND issues a live scope re-read on a
    // second connection per event, OutboxWorker holds a transaction during its
    // upsert, and PollingFallback holds one during its sweep -- so several
    // connections can be in flight at once. 4 could deadlock (a worker holding a
    // tx while it waits for a second connection that the others are holding).
    wikore::Db::init(cfg, /*pool_size=*/8);
    wikore::Redis::init(cfg);

    drogon::app().setTermSignalHandler([] {
        spdlog::info("[wikore-scheduler] SIGTERM received; initiating drain");
        g_shutdown.store(true);
    });
    drogon::app().setIntSignalHandler([] {
        spdlog::info("[wikore-scheduler] SIGINT received; initiating drain");
        g_shutdown.store(true);
    });

    auto shutdown = [] { return g_shutdown.load(); };

    drogon::app().getLoop()->queueInLoop([cfg, shutdown]() {
        drogon::async_run([cfg, shutdown]() -> drogon::Task<void> {
            // -----------------------------------------------------------
            // Registry contract: resolve embedding_models row for the
            // configured model and validate that cfg.embed_dims agrees
            // with the registered dimension. The Qdrant collection name
            // comes from the registry, not from a hard-coded default,
            // so an event recorded as "model X" can never have its
            // vectors land in the wrong collection.
            //
            // Any mismatch is a fail-fast: we'd rather refuse to boot
            // than write vectors that don't match their registry record.
            // We run inside the event loop because wikore::Db::get()
            // requires the drogon framework to be running.
            // -----------------------------------------------------------
            auto db = wikore::Db::get();

            drogon::orm::DbClientPtr partition_db;
            try {
                partition_db = drogon::orm::DbClient::newPgClient(
                    cfg.partition_database_url, 2);
                // Validate the V031 grants by invoking the read-only helper.
                // We deliberately do NOT use has_function_privilege() here:
                // that check is sensitive to whether the login was created
                // with INHERIT, and a defensible NOINHERIT login that uses
                // SET ROLE would falsely fail an inherited-privilege probe.
                // A live SELECT against the SECURITY DEFINER function fails
                // with permission_denied (SQLSTATE 42501) if any of the
                // three function grants are missing, which is exactly the
                // signal we want -- without coupling to INHERIT.
                co_await partition_db->execSqlCoro(
                    "SELECT 1 FROM public.wikore_check_partition_overflow() LIMIT 1");
            } catch (const drogon::orm::DrogonDbException& ex) {
                fail_startup(std::format(
                    "partition database validation failed: {}", ex.base().what()));
                co_return;
            } catch (const std::exception& ex) {
                fail_startup(std::format(
                    "partition database configuration failed: {}", ex.what()));
                co_return;
            }

            std::string qdrant_collection;
            int         registered_dim = 0;
            try {
                auto rows = co_await db->execSqlCoro(
                    "SELECT qdrant_collection, dimension FROM embedding_models "
                    "WHERE name = $1 AND enabled = true",
                    cfg.embed_model);
                if (rows.empty()) {
                    fail_startup(std::format(
                        "embedding model '{}' not found or disabled in "
                        "embedding_models registry", cfg.embed_model));
                    co_return;
                }
                qdrant_collection = rows[0]["qdrant_collection"].as<std::string>();
                registered_dim    = rows[0]["dimension"].as<int>();
            } catch (const drogon::orm::DrogonDbException& ex) {
                fail_startup(std::format(
                    "embedding_models lookup failed: {}", ex.base().what()));
                co_return;
            }

            if (registered_dim != cfg.embed_dims) {
                fail_startup(std::format(
                    "dimension mismatch: cfg.embed_dims={} but "
                    "embedding_models.dimension for '{}' is {} "
                    "(registry is the source of truth)",
                    cfg.embed_dims, cfg.embed_model, registered_dim));
                co_return;
            }

            spdlog::info("[wikore-scheduler] model:   {} (collection={}, dim={})",
                         cfg.embed_model, qdrant_collection, registered_dim);

            auto embedder  = std::make_shared<wikore::rag::LlamaEmbedder>(
                cfg.embed_base_url, cfg.embed_model, registered_dim);
            auto vec_store = std::make_shared<wikore::rag::QdrantVectorStore>(
                cfg.qdrant_url, qdrant_collection);

            // Resync must patch EVERY collection a document has points in (a
            // chunk may be embedded by several models, each with its own
            // collection). This resolver lazily builds and caches a
            // QdrantVectorStore per collection name against the same Qdrant
            // instance; the primary collection reuses the store above. All
            // registered collections live on this Qdrant, so a lookup never
            // fails -- a nullptr return would make ResyncWorker fail the event.
            struct CollectionStores {
                std::mutex mu;
                std::map<std::string, std::shared_ptr<wikore::rag::VectorStorePort>> byName;
            };
            auto stores = std::make_shared<CollectionStores>();
            stores->byName.emplace(qdrant_collection, vec_store);
            wikore::scheduler::ResyncWorker::VectorStoreForCollection store_for_collection =
                [stores, qdrant_url = cfg.qdrant_url](const std::string& collection)
                    -> std::shared_ptr<wikore::rag::VectorStorePort> {
                    std::lock_guard<std::mutex> lk(stores->mu);
                    auto it = stores->byName.find(collection);
                    if (it != stores->byName.end()) return it->second;
                    auto s = std::make_shared<wikore::rag::QdrantVectorStore>(
                        qdrant_url, collection);
                    stores->byName.emplace(collection, s);
                    return s;
                };
            auto doc_repo = std::make_shared<wikore::ingest::PostgresDocumentRepo>(db);

            wikore::scheduler::OutboxWorker::Options outbox_opts;
            outbox_opts.expected_embed_model = cfg.embed_model;

            static wikore::scheduler::OutboxWorker outbox(
                db, embedder, vec_store, shutdown, std::move(outbox_opts));
            static wikore::scheduler::ResyncWorker resync(
                db, store_for_collection, doc_repo, shutdown,
                wikore::scheduler::ResyncWorker::Options{});
            // Edge-cleanup worker consumes qdrant_delete_edge_points events
            // enqueued by V034's BEFORE DELETE trigger on knowledge_edges.
            // Uses its own QdrantVectorStore bound to the edge collection.
            auto edge_vec_store = std::make_shared<wikore::rag::QdrantVectorStore>(
                cfg.qdrant_url, cfg.qdrant_edge_collection);
            static wikore::scheduler::EdgeVectorCleanupWorker edge_cleanup(
                db, edge_vec_store, shutdown,
                wikore::scheduler::EdgeVectorCleanupWorker::Options{});
            // Edge-vector indexing worker consumes qdrant_upsert_edge_vector
            // events enqueued by V037's knowledge_edges_enqueue_upsert_fn
            // trigger. Reads endpoint chunk vectors through the same
            // store_for_collection resolver as ResyncWorker (per-model
            // chunk collections) and writes edge vectors to
            // cfg.qdrant_edge_collection with company_id in the payload —
            // that stamp is what makes PR #53's tenant-scoped delete
            // filter actually enforce something.
            static wikore::scheduler::EmbedEdgeWorker embed_edge(
                db, store_for_collection, edge_vec_store,
                cfg.qdrant_edge_collection, shutdown,
                wikore::scheduler::EmbedEdgeWorker::Options{});
            static wikore::scheduler::PollingFallback polling(
                db, shutdown,
                wikore::scheduler::PollingFallback::Options{});
            static wikore::scheduler::PartitionMaintainer partitions(
                partition_db, shutdown,
                wikore::scheduler::PartitionMaintainer::Options{});

            // Ensure the registry-resolved Qdrant collection exists
            // before any upsert runs. Failure here is fatal: a brief
            // Qdrant outage at startup must not be silently absorbed --
            // upserts that target a non-existent collection would
            // exhaust the retry budget across the entire pending queue.
            if (auto r = co_await vec_store->ensure_collection(registered_dim); !r) {
                fail_startup(std::format(
                    "ensure_collection failed: {} (Qdrant must be "
                    "reachable before draining the outbox)",
                    r.error().message));
                co_return;
            }

            drogon::async_run([]() -> drogon::Task<void> {
                co_await outbox.run();
                on_worker_exit("outbox-worker");
            });
            drogon::async_run([]() -> drogon::Task<void> {
                co_await resync.run();
                on_worker_exit("resync-worker");
            });
            drogon::async_run([]() -> drogon::Task<void> {
                co_await edge_cleanup.run();
                on_worker_exit("edge-cleanup-worker");
            });
            drogon::async_run([]() -> drogon::Task<void> {
                co_await embed_edge.run();
                on_worker_exit("embed-edge-worker");
            });
            drogon::async_run([]() -> drogon::Task<void> {
                co_await polling.run();
                on_worker_exit("polling-fallback");
            });
            drogon::async_run([]() -> drogon::Task<void> {
                co_await partitions.run();
                on_worker_exit("partition-maintainer");
            });
        });
    });

    drogon::app()
        .setThreadNum(2)
        .registerBeginningAdvice([] {
            spdlog::info("[wikore-scheduler] event loop ready");
        })
        .run();

    return g_fatal_exit.load() ? EXIT_FAILURE : EXIT_SUCCESS;
}
