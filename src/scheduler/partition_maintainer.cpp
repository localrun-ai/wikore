#include "wikore/scheduler/partition_maintainer.hpp"
#include "wikore/adapters/postgres/unit_of_work.hpp"
#include <drogon/drogon.h>
#include <spdlog/spdlog.h>
#include <chrono>
#include <format>
#include <stdexcept>

namespace wikore::scheduler {

namespace {

// Coroutine-friendly millisecond sleep using Drogon's event loop timer.
drogon::Task<void> co_sleep_ms(std::chrono::milliseconds d,
                               const std::function<void()>& on_armed)
{
    auto* loop = drogon::app().getLoop();
    struct Awaiter {
        trantor::EventLoop*       loop;
        std::chrono::milliseconds delay;
        const std::function<void()>* on_armed;
        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<> h) {
            loop->runAfter(static_cast<double>(delay.count()) / 1000.0,
                           [h]() mutable { h.resume(); });
            if (*on_armed) {
                try {
                    (*on_armed)();
                } catch (const std::exception& ex) {
                    spdlog::error("[partition-maintainer] sleep observer failed: {}",
                                  ex.what());
                } catch (...) {
                    spdlog::error(
                        "[partition-maintainer] sleep observer failed with "
                        "a non-standard exception");
                }
            }
        }
        void await_resume() const noexcept {}
    };
    co_await Awaiter{loop, d, &on_armed};
}

// Returns current UTC year and month.
std::pair<int,int> utc_year_month()
{
    using namespace std::chrono;
    auto ymd = year_month_day{floor<days>(system_clock::now())};
    return {static_cast<int>(ymd.year()), static_cast<unsigned>(ymd.month())};
}

constexpr std::string_view kAdvisoryLockName = "wikore-partition-maintainer";

} // namespace

PartitionMaintainer::PartitionMaintainer(drogon::orm::DbClientPtr db,
                                         ShutdownPredicate        shutdown_requested,
                                         Options                  opts)
    : db_(std::move(db))
    , shutdown_(std::move(shutdown_requested))
    , opts_(std::move(opts))
{
    if (opts_.sleep_chunk <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("PartitionMaintainer sleep_chunk must be positive");
    }
    if (opts_.interval <= std::chrono::hours::zero()) {
        throw std::invalid_argument("PartitionMaintainer interval must be positive");
    }
    if (opts_.audit_log_lookahead < 0 || opts_.usage_events_lookahead < 0) {
        throw std::invalid_argument("PartitionMaintainer lookahead must be non-negative");
    }
}

// ---------------------------------------------------------------------------
// interruptible_sleep
//
// Sleeps for opts_.interval but wakes every opts_.sleep_chunk to check
// shutdown_. SIGTERM received at the start of the sleep returns in at most one
// chunk (default 1s) instead of up to 24 hours.
// ---------------------------------------------------------------------------

drogon::Task<void> PartitionMaintainer::interruptible_sleep()
{
    const auto deadline =
        std::chrono::steady_clock::now() +
        std::chrono::duration_cast<std::chrono::nanoseconds>(opts_.interval);

    while (!shutdown_()) {
        const auto now      = std::chrono::steady_clock::now();
        if (now >= deadline) break;
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        const auto chunk = std::min(remaining, opts_.sleep_chunk);
        co_await co_sleep_ms(chunk, opts_.on_sleep_armed);
    }
}

// ---------------------------------------------------------------------------
// check_default_overflow
//
// Run outside the advisory lock (read-only; any replica). Logs CRITICAL if
// any rows exist in the catch-all DEFAULT partitions so ops is alerted before
// the window for lossless recovery (detach/move/reattach) shrinks.
// ---------------------------------------------------------------------------

drogon::Task<void> PartitionMaintainer::check_default_overflow()
{
    // Use the SECURITY DEFINER helper (V031) so the runtime role does not
    // need direct SELECT on the child default-partition tables.
    try {
        auto rows = co_await db_->execSqlCoro(
            "SELECT partition_table, has_rows "
            "FROM public.wikore_check_partition_overflow()");
        for (const auto& row : rows) {
            if (row["has_rows"].as<bool>()) {
                spdlog::critical(
                    "[partition-maintainer] OVERFLOW: rows found in {} - "
                    "a named partition was missing at insert time. "
                    "Ops required: DETACH, move rows, ATTACH.",
                    row["partition_table"].as<std::string>());
            }
        }
    } catch (const drogon::orm::DrogonDbException& ex) {
        spdlog::error("[partition-maintainer] overflow check failed: {}",
                      ex.base().what());
    }
}

// ---------------------------------------------------------------------------
// run_once
//
// Acquires a PostgreSQL advisory xact lock (pg_try_advisory_xact_lock) inside
// a UnitOfWork so only one scheduler replica performs DDL per cycle. Non-owners
// skip immediately. The owning replica calls wikore_ensure_partition() (V031
// SECURITY DEFINER) for each expected partition; the function returns TRUE only
// when a partition is actually created, so partitions_created_ is accurate.
// Default-overflow check runs after commit on all replicas (read-only).
// ---------------------------------------------------------------------------

drogon::Task<void> PartitionMaintainer::run_once()
{
    spdlog::info("[partition-maintainer] sweep: audit_log lookahead={}q "
                 "usage_events lookahead={}m",
                 opts_.audit_log_lookahead, opts_.usage_events_lookahead);

    // --- Advisory lock + DDL phase ---
    //
    // No per-statement try/catch inside the loops: a PostgreSQL statement error
    // leaves the transaction aborted, so any subsequent uow.exec would silently
    // fail and uow.commit() would roll back. Catching per-statement would let
    // the code log "created" for partitions that are never actually committed.
    // Instead, let any error propagate to the outer catch which logs and exits
    // the sweep; the next daily cycle retries.
    //
    // "created" names are accumulated locally and logged only after a successful
    // commit so reported counts always match what is actually in the catalog.
    try {
        auto uow = co_await postgres::UnitOfWork::begin(db_);

        auto lock_rows = co_await uow.exec(
            "SELECT pg_try_advisory_xact_lock(hashtext($1)) AS got",
            std::string(kAdvisoryLockName));
        const bool got_lock = !lock_rows.empty() && lock_rows[0]["got"].as<bool>();

        if (!got_lock) {
            spdlog::debug("[partition-maintainer] another replica holds the lock; skipping DDL");
            uow.rollback();
            co_await check_default_overflow();
            co_return;
        }

        {
            std::vector<std::string> created_names;
            auto [year, month] = opts_.now_utc_year_month
                ? opts_.now_utc_year_month()
                : utc_year_month();

            // audit_log: quarterly
            const int quarter = (month - 1) / 3 + 1;
            for (int i = 0; i < opts_.audit_log_lookahead; ++i) {
                int q = quarter + i;
                int y = year + (q - 1) / 4;
                q     = ((q - 1) % 4) + 1;

                std::string label = std::format("audit_log_{}_q{}", y, q);
                spdlog::debug("[partition-maintainer] ensuring {}", label);
                auto r = co_await uow.exec(
                    "SELECT public.wikore_ensure_audit_log_partition($1,$2) AS created",
                    y, q);
                if (!r.empty() && r[0]["created"].as<bool>())
                    created_names.push_back(std::move(label));
            }

            // usage_events: monthly
            for (int i = 0; i < opts_.usage_events_lookahead; ++i) {
                int m = month + i;
                int y = year + (m - 1) / 12;
                m     = ((m - 1) % 12) + 1;

                std::string label = std::format("usage_events_{}_{:02d}", y, m);
                spdlog::debug("[partition-maintainer] ensuring {}", label);
                auto r = co_await uow.exec(
                    "SELECT public.wikore_ensure_usage_events_partition($1,$2) AS created",
                    y, m);
                if (!r.empty() && r[0]["created"].as<bool>())
                    created_names.push_back(std::move(label));
            }

            if (auto r = co_await uow.commit(); !r) {
                spdlog::error("[partition-maintainer] commit failed: {}", r.error().message);
            } else {
                for (const auto& name : created_names)
                    spdlog::info("[partition-maintainer] created {}", name);
                partitions_created_.fetch_add(created_names.size());
                spdlog::info("[partition-maintainer] sweep complete; created={}",
                             created_names.size());
            }
        }
    } catch (const drogon::orm::DrogonDbException& ex) {
        spdlog::error("[partition-maintainer] sweep failed: {}", ex.base().what());
    }

    co_await check_default_overflow();
}

drogon::Task<void> PartitionMaintainer::run()
{
    spdlog::info("[partition-maintainer] starting; interval={}h",
                 opts_.interval.count());
    while (!shutdown_()) {
        co_await run_once();
        if (shutdown_()) break;
        co_await interruptible_sleep();
    }
    spdlog::info("[partition-maintainer] stopped");
}

} // namespace wikore::scheduler
