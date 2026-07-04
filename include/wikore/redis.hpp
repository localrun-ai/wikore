#pragma once
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <chrono>
#include "wikore/config.hpp"

namespace wikore {

// Thread-safe synchronous Redis client backed by a fixed connection pool.
// Silently degrades (nullopt / no-op) when Redis is unavailable or not
// configured. All operations use a 2-second socket timeout.
//
// Design note: synchronous hiredis is deliberate. Auth cache hits take ~0.1ms
// on localhost - negligible against the LLM latency budget. Async hiredis
// requires non-trivial Drogon event-loop bridging with no measurable benefit
// for this access pattern.
struct Redis {
    // Parse REDIS_URL and create a pool of `pool_size` connections.
    // No-op (warns) if cfg.redis_url is empty.
    static void init(const Config& cfg, int pool_size = 8);

    static std::optional<std::string> get(std::string_view key);

    // ttl == 0 means no expiry (plain SET).
    static void set(std::string_view key, std::string_view value,
                    std::chrono::seconds ttl = {});

    static void del(std::string_view key);

    // --- List operations for the per-tenant ingest queue ---------------------

    // Push to the left (head) of `key`. Returns the new list length, or
    // -1 on error / no pool.
    static long long lpush(std::string_view key, std::string_view value);

    // Pop from the right (tail) of `key`. Returns std::nullopt if the list
    // is empty, the key is missing, or Redis is unavailable.
    static std::optional<std::string> rpop(std::string_view key);

    // Atomically move the tail of `src` to the head of `dst` and return
    // the moved value. The per-worker processing-list pattern uses this
    // so a crashed worker leaves the in-flight job on `dst` for the
    // sweep reaper to requeue. Returns std::nullopt if `src` was empty.
    static std::optional<std::string>
    lmove_right_left(std::string_view src, std::string_view dst);

    // Remove the first `count` occurrences of `value` from `key`. Returns
    // the number removed, or -1 on error / no pool. Called by a worker on
    // successful processing to drop the entry from its processing list.
    static long long lrem(std::string_view key, long long count,
                          std::string_view value);

    // Atomically remove `payload` from `src` (LREM count=1) and, if and
    // only if the LREM removed something, LPUSH it to `dst`. Implemented
    // via EVAL so concurrent schedulers cannot both LPUSH the same item:
    // only the scheduler whose LREM observes count=1 LPUSHes.
    //
    // Returns:
    //   * 1  -> transfer succeeded
    //   * 0  -> entry was not present on `src` (another sweeper got it)
    //   * -1 -> Redis error / no pool
    static int transfer_proc_to_source(std::string_view src,
                                       std::string_view dst,
                                       std::string_view payload);

    // Return the contents of `key` in [start, stop]. Stop = -1 means end.
    // Used by the sweep reaper to inspect orphaned processing lists.
    static std::vector<std::string> lrange(std::string_view key,
                                           long long start, long long stop);

    // EXISTS check (1 if key exists, 0 otherwise; -1 on error / no pool).
    // Used by the sweep reaper to detect crashed workers via missing
    // heartbeat keys.
    static int exists(std::string_view key);

    // Cursor-paged scan returning keys matching `pattern`. Bounded `limit`
    // caps the number of keys returned to keep the function predictable.
    // Uses SCAN MATCH (NOT KEYS) so the call is production-safe.
    static std::vector<std::string> scan_keys(std::string_view pattern,
                                              size_t limit = 1024);

    // --- Concurrency semaphore (lease-based, crash-safe) --------------------

    // Try to acquire one of at most `cap` slots on `key`. Implemented as a
    // ZSET of lease tokens scored by their expiry (server-now + lease_ttl_ms):
    // expired leases (from crashed holders) are pruned on every acquire, so a
    // slot is never leaked permanently. The clock is read from redis.call
    // ('TIME') inside the Lua script, so every app node shares ONE clock -
    // client clock skew cannot mis-prune another node's active lease. `token`
    // must be unique per holder and is passed back to sem_release. Returns:
    //   * 1  -> acquired (caller now holds a slot)
    //   * 0  -> at capacity
    //   * -1 -> Redis error / no pool (caller decides fail-open vs closed)
    static int sem_acquire(std::string_view key, int cap,
                           long long lease_ttl_ms, std::string_view token);

    // Release the slot held under `token` (ZREM). Idempotent; safe if the
    // lease already expired.
    static void sem_release(std::string_view key, std::string_view token);

    // --- Token-bucket rate limiter ------------------------------------------

    // Atomically refill and take `cost` tokens from the bucket on `key`.
    // The bucket refills at `refill_per_sec` up to `burst` capacity. The clock
    // is read from redis.call('TIME') inside Lua (single shared clock), so
    // skewed app nodes cannot repeatedly over-refill the bucket. Returns:
    //   * 1  -> allowed (tokens deducted)
    //   * 0  -> denied (insufficient tokens)
    //   * -1 -> Redis error / no pool
    static int token_bucket_take(std::string_view key, double refill_per_sec,
                                 int burst, int cost);
};

} // namespace wikore
