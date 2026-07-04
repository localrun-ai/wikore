#pragma once
#include <chrono>
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
};

// Added to a slot's max hold time to form its lease TTL, so the lease always
// outlives the LLM call it guards (covers the gap between call timeout and the
// RAII release, plus clock jitter). Also bounds the crash-reclaim window.
inline constexpr std::chrono::seconds kLeaseMargin{15};

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
    // Throws std::invalid_argument if any limit is out of range
    // (max_concurrency >= 1, rate_per_sec > 0, burst >= 1, lease_ttl_ms >= 1):
    // a zero/negative value would either disable protection or make the Lua
    // arithmetic (e.g. burst/rate) fault, which fail-open would then silently
    // ignore. Validate once at startup instead.
    explicit LlmGate(LlmLimits limits);

    // Token-bucket check. true = within the tenant's rate (or Redis down).
    bool allow_rate(std::string_view company_id) const;

    // Take a concurrency slot for a call expected to hold it for at most
    // `max_hold`. The lease TTL is derived as max_hold + kLeaseMargin, so a
    // crashed holder's slot is reclaimed only AFTER the call could no longer be
    // running - a lease can never expire under a live call, which would let
    // concurrency exceed the cap. Callers pass their LLM request timeout as
    // max_hold. Returns a held lease on success; nullopt when the tenant is at
    // max_concurrency. On Redis error the optional is engaged with a
    // non-releasing lease (fail-open).
    //
    // Throws std::invalid_argument if max_hold is outside (0, 24h] - clamping a
    // longer declared hold would let the lease expire under a live call.
    std::optional<LlmLease> acquire(std::string_view       company_id,
                                    std::chrono::milliseconds max_hold) const;

    const LlmLimits& limits() const noexcept { return limits_; }

private:
    LlmLimits limits_;
};

} // namespace wikore::rag
