#include <catch2/catch_test_macros.hpp>
#include "wikore/rag/llm_gate.hpp"
#include "wikore/redis.hpp"
#include "wikore/config.hpp"

#include <cstdlib>
#include <string>

// Tests for the per-tenant LLM guardrails (concurrency semaphore + token-bucket
// rate limiter). The Redis primitives take an explicit now_ms, so refill and
// lease-expiry are driven deterministically. Skips without REDIS_URL.

namespace {

bool redis_available() { return std::getenv("REDIS_URL") != nullptr; }

void init_redis()
{
    wikore::Config cfg;
    if (const char* r = std::getenv("REDIS_URL")) cfg.redis_url = r;
    wikore::Redis::init(cfg);
}

} // namespace

TEST_CASE("sem_acquire: caps concurrency and reclaims expired leases", "[redis][llm-gate]")
{
    if (!redis_available()) SKIP("REDIS_URL not set");
    init_redis();
    const std::string k = "lr:test:sem:cap";
    wikore::Redis::del(k);

    // cap = 2, lease ttl = 100 ms, all at t=1000.
    CHECK(wikore::Redis::sem_acquire(k, 2, 100, "A", 1000) == 1);
    CHECK(wikore::Redis::sem_acquire(k, 2, 100, "B", 1000) == 1);
    CHECK(wikore::Redis::sem_acquire(k, 2, 100, "C", 1000) == 0);   // at capacity
    wikore::Redis::sem_release(k, "A");
    CHECK(wikore::Redis::sem_acquire(k, 2, 100, "C", 1000) == 1);   // slot freed

    // Crash-safe reclaim: cap = 1. A acquired at t=1000 expires at t=1100.
    wikore::Redis::del(k);
    CHECK(wikore::Redis::sem_acquire(k, 1, 100, "A", 1000) == 1);
    CHECK(wikore::Redis::sem_acquire(k, 1, 100, "B", 1050) == 0);   // A still active
    CHECK(wikore::Redis::sem_acquire(k, 1, 100, "C", 1200) == 1);   // A expired -> pruned
    wikore::Redis::del(k);
}

TEST_CASE("token_bucket_take: meters burst and refills over time", "[redis][llm-gate]")
{
    if (!redis_available()) SKIP("REDIS_URL not set");
    init_redis();
    const std::string k = "lr:test:rate:bucket";
    wikore::Redis::del(k);

    // burst = 3, rate = 1 token/sec.
    CHECK(wikore::Redis::token_bucket_take(k, 1.0, 3, 1, 0) == 1);
    CHECK(wikore::Redis::token_bucket_take(k, 1.0, 3, 1, 0) == 1);
    CHECK(wikore::Redis::token_bucket_take(k, 1.0, 3, 1, 0) == 1);
    CHECK(wikore::Redis::token_bucket_take(k, 1.0, 3, 1, 0) == 0);   // empty

    CHECK(wikore::Redis::token_bucket_take(k, 1.0, 3, 1, 1000) == 1); // +1 after 1s
    CHECK(wikore::Redis::token_bucket_take(k, 1.0, 3, 1, 1000) == 0);

    // 10s later refills but is capped at burst (3), not 10.
    CHECK(wikore::Redis::token_bucket_take(k, 1.0, 3, 1, 11000) == 1);
    CHECK(wikore::Redis::token_bucket_take(k, 1.0, 3, 1, 11000) == 1);
    CHECK(wikore::Redis::token_bucket_take(k, 1.0, 3, 1, 11000) == 1);
    CHECK(wikore::Redis::token_bucket_take(k, 1.0, 3, 1, 11000) == 0);
    wikore::Redis::del(k);
}

TEST_CASE("LlmGate::acquire: per-tenant concurrency with RAII release", "[redis][llm-gate]")
{
    if (!redis_available()) SKIP("REDIS_URL not set");
    init_redis();
    wikore::Redis::del("lr:llm:sem:tenA");
    wikore::rag::LlmGate gate(wikore::rag::LlmLimits{
        .max_concurrency = 2, .rate_per_sec = 100, .burst = 100, .lease_ttl_ms = 60000});

    auto l1 = gate.acquire("tenA");  REQUIRE(l1);  CHECK(l1->holds_slot());
    auto l2 = gate.acquire("tenA");  REQUIRE(l2);
    CHECK_FALSE(gate.acquire("tenA"));             // at capacity

    l1.reset();                                    // RAII release of the slot
    auto l4 = gate.acquire("tenA");  CHECK(l4);    // freed slot re-acquirable
    wikore::Redis::del("lr:llm:sem:tenA");
}

TEST_CASE("LlmGate: one tenant's limit does not affect another", "[redis][llm-gate]")
{
    if (!redis_available()) SKIP("REDIS_URL not set");
    init_redis();
    wikore::Redis::del("lr:llm:sem:tX");
    wikore::Redis::del("lr:llm:sem:tY");
    wikore::rag::LlmGate gate(wikore::rag::LlmLimits{
        .max_concurrency = 1, .rate_per_sec = 100, .burst = 100, .lease_ttl_ms = 60000});

    auto x = gate.acquire("tX");  REQUIRE(x);
    CHECK_FALSE(gate.acquire("tX"));               // tX at cap
    auto y = gate.acquire("tY");  CHECK(y);        // tY independent
    wikore::Redis::del("lr:llm:sem:tX");
    wikore::Redis::del("lr:llm:sem:tY");
}

TEST_CASE("LlmGate::allow_rate: burst then denial", "[redis][llm-gate]")
{
    if (!redis_available()) SKIP("REDIS_URL not set");
    init_redis();
    wikore::Redis::del("lr:llm:rate:tR");
    // rate 1/sec, burst 3: three quick calls pass (no meaningful refill in <1ms),
    // the fourth is denied.
    wikore::rag::LlmGate gate(wikore::rag::LlmLimits{
        .max_concurrency = 100, .rate_per_sec = 1.0, .burst = 3, .lease_ttl_ms = 60000});

    CHECK(gate.allow_rate("tR"));
    CHECK(gate.allow_rate("tR"));
    CHECK(gate.allow_rate("tR"));
    CHECK_FALSE(gate.allow_rate("tR"));
    wikore::Redis::del("lr:llm:rate:tR");
}
