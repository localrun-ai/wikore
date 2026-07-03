#include "wikore/application/ingest_document_version.hpp"
#include "wikore/config.hpp"
#include "wikore/db.hpp"
#include "wikore/ingest/chunker.hpp"
#include "wikore/ingest/document_repo.hpp"
#include "wikore/ingest/parser.hpp"
#include "wikore/ingest/worker.hpp"
#include "wikore/redis.hpp"
#include <drogon/drogon.h>
#include <spdlog/spdlog.h>
#include <atomic>
#include <cstdlib>

// ---------------------------------------------------------------------------
// wikore-ingest: per-tenant fair Redis-queue consumer.
//
// On startup, builds the production adapter wiring (PostgresDocumentRepo +
// PlainTextParser + Chunker) and the IngestWorker, then spawns the worker's
// run() coroutine on the Drogon event loop.
//
// Iteration 1 scope: the use case writes chunks + audit + outbox_event in
// one Postgres transaction; the qdrant_upsert_chunk_payload event is then
// drained by wikore-scheduler. Real PDF/DOCX/HTML parsers are a follow-up
// (PlainTextParser is the placeholder that satisfies ParserPort today).
//
// SIGTERM/SIGINT are routed through drogon::app().setTermSignalHandler /
// setIntSignalHandler (NOT std::signal, which drogon overwrites in run()).
// They flip g_shutdown so the worker's idle backoff returns promptly; the
// worker's natural exit then triggers drogon::app().quit() so the process
// terminates only after the in-flight job has drained.
// ---------------------------------------------------------------------------

namespace {
std::atomic<bool> g_shutdown{false};
} // namespace

// Fatal-exit channel: the worker's run() coroutine sets this BEFORE
// calling drogon::app().quit() if it exited in fatal state. main()
// reads it after app().run() returns and propagates as a non-zero
// status so systemd `Restart=on-failure` (and equivalent policies)
// restart the process instead of treating it as a clean shutdown.
namespace {
std::atomic<bool> g_fatal_exit{false};
}

int main()
{
    const auto cfg = wikore::Config::from_env();

    spdlog::info("[wikore-ingest] version: " WIKORE_GIT_HASH);
    spdlog::info("[wikore-ingest] db:      {}", cfg.database_url);
    spdlog::info("[wikore-ingest] redis:   {}", cfg.redis_url);
    spdlog::info("[wikore-ingest] embed:   {}", cfg.embed_base_url);

    wikore::Db::init(cfg, /*pool_size=*/4);
    wikore::Redis::init(cfg);

    // Replace drogon's default SIGTERM/SIGINT quit-immediately behaviour
    // with a graceful drain. Drogon's app().run() installs its own signal
    // handlers, so std::signal(...) would be overwritten and the worker's
    // drain path would never execute on real SIGTERM.
    drogon::app().setTermSignalHandler([] {
        spdlog::info("[wikore-ingest] SIGTERM received; initiating drain");
        g_shutdown.store(true);
    });
    drogon::app().setIntSignalHandler([] {
        spdlog::info("[wikore-ingest] SIGINT received; initiating drain");
        g_shutdown.store(true);
    });

    auto parser = std::make_shared<wikore::ingest::DispatchingParser>();

    // The worker is constructed inside queueInLoop because Db::get() is
    // only valid once drogon's framework has initialised. Static so the
    // coroutine's reference outlives the lambda.
    static std::unique_ptr<wikore::ingest::IngestWorker> worker;

    drogon::app().getLoop()->queueInLoop([parser]() {
        auto db_   = wikore::Db::get();
        auto repo  = std::make_shared<wikore::ingest::PostgresDocumentRepo>(db_);
        wikore::ingest::Chunker chunker;

        wikore::application::IngestDocumentVersionUseCase use_case(
            db_, std::move(repo), parser, std::move(chunker));

        worker = std::make_unique<wikore::ingest::IngestWorker>(
            std::move(use_case),
            [] { return g_shutdown.load(); },
            wikore::ingest::IngestWorker::Options{});

        drogon::async_run([]() -> drogon::Task<void> {
            co_await worker->run();
            // Worker's run() returns when either (a) g_shutdown is true
            // and the current rotation is complete (clean shutdown, exit
            // status 0), or (b) IngestWorker::fatal_failure_ is set
            // because all transfer-back attempts to Redis failed (fatal,
            // exit status non-zero so the supervisor restarts).
            if (worker->fatal_failure()) {
                spdlog::error("[wikore-ingest] worker exited in FATAL "
                              "state; propagating non-zero status so "
                              "the supervisor restarts this process");
                g_fatal_exit.store(true);
            } else {
                spdlog::info("[wikore-ingest] worker exited; quitting drogon");
            }
            drogon::app().quit();
        });
    });

    drogon::app()
        .setThreadNum(2)
        .registerBeginningAdvice([] {
            spdlog::info("[wikore-ingest] event loop ready");
        })
        .run();

    return g_fatal_exit.load() ? EXIT_FAILURE : EXIT_SUCCESS;
}
