#include "wikore/rag/llm_gate.hpp"
#include "wikore/redis.hpp"
#include "wikore/config.hpp"
#include "wikore/domain/types.hpp"   // uuid_generate

#include <spdlog/spdlog.h>
#include <algorithm>
#include <cmath>
#include <format>
#include <stdexcept>

namespace wikore::rag {

namespace {

std::string sem_key(std::string_view cid)  { return "lr:llm:sem:"  + std::string(cid); }
std::string rate_key(std::string_view cid) { return "lr:llm:rate:" + std::string(cid); }

} // namespace

LlmGate::LlmGate(LlmLimits limits) : limits_(limits)
{
    if (limits_.max_concurrency < 1)
        throw std::invalid_argument(std::format(
            "LlmGate: max_concurrency must be >= 1 (got {})", limits_.max_concurrency));
    // isfinite rejects NaN/inf: NaN <= 0 is false, so a NaN rate would slip
    // past a bare comparison and then fault the burst/rate Lua arithmetic.
    if (!std::isfinite(limits_.rate_per_sec) || limits_.rate_per_sec <= 0.0)
        throw std::invalid_argument(std::format(
            "LlmGate: rate_per_sec must be finite and > 0 (got {})", limits_.rate_per_sec));
    if (limits_.burst < 1)
        throw std::invalid_argument(std::format(
            "LlmGate: burst must be >= 1 (got {})", limits_.burst));
}

LlmLimits llm_limits_from_config(const Config& cfg)
{
    return LlmLimits{
        .max_concurrency = cfg.llm_concurrency,
        .rate_per_sec    = cfg.llm_rate_per_sec,
        .burst           = cfg.llm_rate_burst,
    };
}

// ---------------------------------------------------------------------------
// LlmLease
// ---------------------------------------------------------------------------

LlmLease::~LlmLease() { release(); }

LlmLease& LlmLease::operator=(LlmLease&& o) noexcept
{
    if (this != &o) {
        release();
        sem_key_ = std::move(o.sem_key_);
        token_   = std::move(o.token_);
        o.token_.clear();
    }
    return *this;
}

void LlmLease::release() noexcept
{
    if (!token_.empty()) {
        Redis::sem_release(sem_key_, token_);
        token_.clear();
    }
}

// ---------------------------------------------------------------------------
// LlmGate
// ---------------------------------------------------------------------------

bool LlmGate::allow_rate(std::string_view company_id) const
{
    const int r = Redis::token_bucket_take(
        rate_key(company_id), limits_.rate_per_sec, limits_.burst, /*cost=*/1);
    if (r < 0) {
        spdlog::warn("[llm-gate] rate limiter unavailable for {}; failing open", company_id);
        return true;   // fail-open
    }
    return r == 1;
}

std::optional<LlmLease> LlmGate::acquire(std::string_view          company_id,
                                         std::chrono::milliseconds max_hold) const
{
    const std::string key   = sem_key(company_id);
    std::string       token = uuid_generate();

    // Lease = clamped max_hold + margin, computed in int64 (no overflow): the
    // slot's lease always outlives the call it guards.
    constexpr long long kMaxHoldMs = 24LL * 60 * 60 * 1000;   // 24h ceiling
    const long long hold_ms  = std::clamp<long long>(max_hold.count(), 0, kMaxHoldMs);
    const long long lease_ms = hold_ms +
        std::chrono::duration_cast<std::chrono::milliseconds>(kLeaseMargin).count();

    const int r = Redis::sem_acquire(
        key, limits_.max_concurrency, lease_ms, token);
    if (r == 1)
        return LlmLease{key, std::move(token)};
    if (r == 0)
        return std::nullopt;               // at capacity

    spdlog::warn("[llm-gate] concurrency semaphore unavailable for {}; failing open",
                 company_id);
    return LlmLease{};                      // fail-open: engaged, holds no slot
}

} // namespace wikore::rag
