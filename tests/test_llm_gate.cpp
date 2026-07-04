#include <catch2/catch_test_macros.hpp>
#include "wikore/rag/llm_gate.hpp"
#include "wikore/redis.hpp"
#include "wikore/config.hpp"

#include <chrono>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>

// Tests for the per-tenant LLM guardrails (concurrency semaphore + token-bucket
// rate limiter). The clock lives in Redis (redis.call('TIME')), so time-based
// behavior (lease expiry, refill) is exercised with real elapsed time via short
// sleeps. Redis-backed cases skip without REDIS_URL; config validation does not
// need Redis.

namespace {

bool redis_available() { return std::getenv("REDIS_URL") != nullptr; }

void init_redis()
{
    wikore::Config cfg;
    if (const char* r = std::getenv("REDIS_URL")) cfg.redis_url = r;
    wikore::Redis::init(cfg);
}

void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

} // namespace

TEST_CASE("sem_acquire: caps concurrency and reclaims expired leases", "[redis][llm-gate]")
{
    if (!redis_available()) SKIP("REDIS_URL not set");
    init_redis();
    const std::string k = "lr:test:sem:cap";
    wikore::Redis::del(k);

    // cap = 2, long lease so nothing expires mid-test.
    CHECK(wikore::Redis::sem_acquire(k, 2, 60000, "A") == 1);
    CHECK(wikore::Redis::sem_acquire(k, 2, 60000, "B") == 1);
    CHECK(wikore::Redis::sem_acquire(k, 2, 60000, "C") == 0);   // at capacity
    wikore::Redis::sem_release(k, "A");
    CHECK(wikore::Redis::sem_acquire(k, 2, 60000, "C") == 1);   // slot freed
    wikore::Redis::del(k);

    // Crash-safe reclaim: cap = 1, 100 ms lease. A is never released; after the
    // lease elapses a later acquire prunes it and admits.
    CHECK(wikore::Redis::sem_acquire(k, 1, 100, "A") == 1);
    CHECK(wikore::Redis::sem_acquire(k, 1, 100, "B") == 0);     // A still active
    sleep_ms(200);
    CHECK(wikore::Redis::sem_acquire(k, 1, 100, "C") == 1);     // A expired -> pruned
    wikore::Redis::del(k);
}

TEST_CASE("token_bucket_take: meters burst and refills over time", "[redis][llm-gate]")
{
    if (!redis_available()) SKIP("REDIS_URL not set");
    init_redis();
    const std::string k = "lr:test:rate:bucket";
    wikore::Redis::del(k);

    // burst = 2, rate = 100 tokens/sec.
    CHECK(wikore::Redis::token_bucket_take(k, 100.0, 2, 1) == 1);
    CHECK(wikore::Redis::token_bucket_take(k, 100.0, 2, 1) == 1);
    CHECK(wikore::Redis::token_bucket_take(k, 100.0, 2, 1) == 0);   // empty (refill negligible)
    sleep_ms(100);                                                  // +~10 tokens, capped at 2
    CHECK(wikore::Redis::token_bucket_take(k, 100.0, 2, 1) == 1);   // refilled
    wikore::Redis::del(k);
}

TEST_CASE("LlmGate::acquire: per-tenant concurrency with RAII release", "[redis][llm-gate]")
{
    if (!redis_available()) SKIP("REDIS_URL not set");
    init_redis();
    wikore::Redis::del("lr:llm:sem:tenA");
    wikore::rag::LlmGate gate(wikore::rag::LlmLimits{
        .max_concurrency = 2, .rate_per_sec = 100, .burst = 100});

    auto l1 = gate.acquire("tenA", std::chrono::seconds(30));  REQUIRE(l1);  CHECK(l1->holds_slot());
    auto l2 = gate.acquire("tenA", std::chrono::seconds(30));  REQUIRE(l2);
    CHECK_FALSE(gate.acquire("tenA", std::chrono::seconds(30)));             // at capacity

    l1.reset();                                    // RAII release of the slot
    auto l4 = gate.acquire("tenA", std::chrono::seconds(30));  CHECK(l4);    // freed slot re-acquirable
    wikore::Redis::del("lr:llm:sem:tenA");
}

TEST_CASE("LlmGate: one tenant's limit does not affect another", "[redis][llm-gate]")
{
    if (!redis_available()) SKIP("REDIS_URL not set");
    init_redis();
    wikore::Redis::del("lr:llm:sem:tX");
    wikore::Redis::del("lr:llm:sem:tY");
    wikore::rag::LlmGate gate(wikore::rag::LlmLimits{
        .max_concurrency = 1, .rate_per_sec = 100, .burst = 100});

    auto x = gate.acquire("tX", std::chrono::seconds(30));  REQUIRE(x);
    CHECK_FALSE(gate.acquire("tX", std::chrono::seconds(30)));               // tX at cap
    auto y = gate.acquire("tY", std::chrono::seconds(30));  CHECK(y);        // tY independent
    wikore::Redis::del("lr:llm:sem:tX");
    wikore::Redis::del("lr:llm:sem:tY");
}

TEST_CASE("LlmGate::allow_rate: burst then denial", "[redis][llm-gate]")
{
    if (!redis_available()) SKIP("REDIS_URL not set");
    init_redis();
    wikore::Redis::del("lr:llm:rate:tR");
    // rate 1/sec, burst 3: three quick calls pass (refill negligible in the few
    // ms they take), the fourth is denied.
    wikore::rag::LlmGate gate(wikore::rag::LlmLimits{
        .max_concurrency = 100, .rate_per_sec = 1.0, .burst = 3});

    CHECK(gate.allow_rate("tR"));
    CHECK(gate.allow_rate("tR"));
    CHECK(gate.allow_rate("tR"));
    CHECK_FALSE(gate.allow_rate("tR"));
    wikore::Redis::del("lr:llm:rate:tR");
}

TEST_CASE("LlmGate: rejects invalid limit configuration", "[llm-gate]")
{
    using wikore::rag::LlmGate;
    using wikore::rag::LlmLimits;
    const LlmLimits ok{.max_concurrency = 4, .rate_per_sec = 5.0, .burst = 15};

    CHECK_NOTHROW(LlmGate{ok});

    auto bad = [&](auto mutate) {
        LlmLimits l = ok; mutate(l);
        CHECK_THROWS_AS(LlmGate{l}, std::invalid_argument);
    };
    bad([](LlmLimits& l){ l.max_concurrency = 0; });
    bad([](LlmLimits& l){ l.max_concurrency = -1; });
    bad([](LlmLimits& l){ l.rate_per_sec = 0.0; });
    bad([](LlmLimits& l){ l.rate_per_sec = -0.5; });
    bad([](LlmLimits& l){ l.rate_per_sec = std::numeric_limits<double>::quiet_NaN(); });
    bad([](LlmLimits& l){ l.rate_per_sec = std::numeric_limits<double>::infinity(); });
    bad([](LlmLimits& l){ l.burst = 0; });

    // max_hold is validated in acquire() before any Redis call, so an
    // out-of-range hold is rejected (never silently clamped).
    LlmGate g{ok};
    CHECK_THROWS_AS(g.acquire("c", std::chrono::milliseconds(0)), std::invalid_argument);
    CHECK_THROWS_AS(g.acquire("c", std::chrono::seconds(-1)),     std::invalid_argument);
    CHECK_THROWS_AS(g.acquire("c", std::chrono::hours(25)),       std::invalid_argument);
}
