#pragma once
#include <optional>
#include <string>
#include <string_view>

namespace wikore { struct Config; }

namespace wikore::rag {

// ---------------------------------------------------------------------------
// Per-tenant LLM guardrails (Iteration 3).
//
// Two independent limits, both enforced in Redis and both keyed by company_id
// so one tenant cannot starve another:
//   * a CONCURRENCY semaphore  (lr:llm:sem:{cid})  - at most N in-flight calls
//   * a RATE limiter (token bucket, lr:llm:rate:{cid}) - sustained request rate
//
// Fail-open: if Redis is unavailable the limits are not enforced (a request
// proceeds) rather than taking chat down. This matches the Redis layer's
// "silently degrades" posture; the limits protect the LLM backend under load,
// they are not a security control.
// ---------------------------------------------------------------------------

struct LlmLimits {
    int    max_concurrency = 4;        // in-flight LLM calls per tenant
    double rate_per_sec    = 5.0;      // sustained request rate per tenant
    int    burst           = 15;       // token-bucket capacity per tenant
    int    lease_ttl_ms    = 120000;   // max slot hold time (crash reclaim)
};

LlmLimits llm_limits_from_config(const wikore::Config& cfg);

// RAII lease: releases the concurrency slot on destruction. Move-only; a
// default-constructed lease holds nothing (used for the fail-open path).
class LlmLease {
public:
    LlmLease() = default;
    LlmLease(std::string sem_key, std::string token)
        : sem_key_(std::move(sem_key)), token_(std::move(token)) {}
    ~LlmLease();

    LlmLease(LlmLease&& o) noexcept
        : sem_key_(std::move(o.sem_key_)), token_(std::move(o.token_)) { o.token_.clear(); }
    LlmLease& operator=(LlmLease&& o) noexcept;
    LlmLease(const LlmLease&)            = delete;
    LlmLease& operator=(const LlmLease&) = delete;

    // True when this lease holds a real (releasable) slot. A fail-open lease
    // is engaged in the optional but holds nothing, so this is false.
    bool holds_slot() const noexcept { return !token_.empty(); }

private:
    std::string sem_key_;
    std::string token_;
    void release() noexcept;
};

// Stateless; construct once from config and share across requests.
class LlmGate {
public:
    explicit LlmGate(LlmLimits limits) : limits_(limits) {}

    // Token-bucket check. true = within the tenant's rate (or Redis down).
    bool allow_rate(std::string_view company_id) const;

    // Take a concurrency slot. Returns a held lease on success; nullopt when
    // the tenant is at max_concurrency. On Redis error the optional is engaged
    // with a non-releasing lease (fail-open).
    std::optional<LlmLease> acquire(std::string_view company_id) const;

    const LlmLimits& limits() const noexcept { return limits_; }

private:
    LlmLimits limits_;
};

} // namespace wikore::rag
