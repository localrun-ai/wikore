#pragma once
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <atomic>
#include <chrono>
#include <functional>
#include <utility>

namespace wikore::scheduler {

// ---------------------------------------------------------------------------
// PartitionMaintainer - ensures time-partitioned tables never run out of
// partitions and alerts if rows fall into the catch-all DEFAULT partition.
//
// Two tables are maintained:
//   audit_log    (V007) - quarterly partitions, named audit_log_YYYY_qN
//   usage_events (V016) - monthly partitions, named usage_events_YYYY_MM
//
// Both tables have a *_default catch-all that prevents INSERT failures, but
// rows landing there cannot be moved to named partitions without a locking
// maintenance op. This job runs ahead of the frontier and alerts immediately
// if any overflow is detected.
//
// Runs once at startup and then every `interval` (default 24h). A daily
// run with a lookahead of 4 quarters / 12 months keeps at least a year of
// partitions pre-created at all times.
// ---------------------------------------------------------------------------

class PartitionMaintainer {
public:
    using ShutdownPredicate = std::function<bool()>;

    struct Options {
        std::chrono::hours        interval               = std::chrono::hours(24);
        int                       audit_log_lookahead    = 4;    // quarters
        int                       usage_events_lookahead = 12;   // months
        // Wake interval during the inter-sweep sleep. Shorter values let the
        // process respond to shutdown sooner (at the cost of more timer events).
        // 1s bounds daemon-shutdown latency to ~1s without meaningfully more
        // timer churn (86400 wakes/day is negligible). Tests use 100ms so the
        // shutdown test completes in under 1 second.
        std::chrono::milliseconds sleep_chunk            = std::chrono::seconds(1);
        // Optional observer invoked after an inter-sweep timer is armed.
        std::function<void()> on_sleep_armed;
        // Override the reference date for sweeps. Null = use UTC system clock.
        // Tests inject a fixed pair<year,month> to make assertions time-independent.
        std::function<std::pair<int,int>()> now_utc_year_month;
    };

    PartitionMaintainer(drogon::orm::DbClientPtr db,
                        ShutdownPredicate        shutdown_requested,
                        Options                  opts);

    drogon::Task<void> run();

    // Single sweep under the advisory lock. Creates missing partitions via
    // wikore_ensure_partition() (V031 SECURITY DEFINER function) and checks
    // for default-partition overflow. Non-owning replicas skip silently.
    // Exposed for testing.
    drogon::Task<void> run_once();

    // Counts partitions actually created (wikore_ensure_partition returned
    // TRUE). IF NOT EXISTS no-ops are not counted.
    std::size_t partitions_created() const { return partitions_created_.load(); }

private:
    drogon::Task<void> check_default_overflow();
    drogon::Task<void> interruptible_sleep();

    drogon::orm::DbClientPtr db_;
    ShutdownPredicate        shutdown_;
    Options                  opts_;
    std::atomic<std::size_t> partitions_created_{0};
};

} // namespace wikore::scheduler
