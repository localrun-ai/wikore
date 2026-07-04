#include "wikore/redis.hpp"
#include <hiredis/hiredis.h>
#include <spdlog/spdlog.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace wikore {

namespace {

struct Slot {
    std::mutex    mu;
    redisContext* ctx = nullptr;
};

struct Pool {
    std::vector<std::unique_ptr<Slot>> slots;
    std::string host     = "127.0.0.1";
    std::string password;
    int         port     = 6379;
    int         db       = 0;
    std::atomic<size_t> rr{0};

    redisContext* connect_one() const {
        timeval tv{2, 0};
        redisContext* c = redisConnectWithTimeout(host.c_str(), port, tv);
        if (!c || c->err) {
            std::string e = c ? c->errstr : "null context";
            if (c) redisFree(c);
            throw std::runtime_error(std::string("[redis] connect failed: ") + e);
        }
        redisSetTimeout(c, tv);
        if (!password.empty()) {
            auto* r = static_cast<redisReply*>(
                redisCommand(c, "AUTH %s", password.c_str()));
            if (r) freeReplyObject(r);
        }
        if (db != 0) {
            auto* r = static_cast<redisReply*>(redisCommand(c, "SELECT %d", db));
            if (r) freeReplyObject(r);
        }
        return c;
    }
};

static std::unique_ptr<Pool> g_pool;

static void parse_redis_url(Pool& p, const std::string& url) {
    if (url.rfind("redis://", 0) != 0) {
        spdlog::warn("[redis] unexpected URL scheme, using defaults");
        return;
    }
    std::string rest = url.substr(8); // after "redis://"

    // Extract optional ":password@" or "user:password@"
    auto at = rest.rfind('@');
    if (at != std::string::npos) {
        auto colon = rest.find(':', 0);
        if (colon != std::string::npos && colon < at)
            p.password = rest.substr(colon + 1, at - colon - 1);
        rest = rest.substr(at + 1);
    }

    // host[:port][/db]
    auto slash = rest.find('/');
    std::string hostport = rest.substr(0, slash);
    if (slash != std::string::npos && slash + 1 < rest.size())
        p.db = std::stoi(rest.substr(slash + 1));

    auto colon = hostport.rfind(':');
    if (colon != std::string::npos) {
        p.host = hostport.substr(0, colon);
        p.port = std::stoi(hostport.substr(colon + 1));
    } else {
        p.host = hostport;
    }
}

static Slot* pick_slot() {
    if (!g_pool || g_pool->slots.empty()) return nullptr;
    size_t idx = g_pool->rr.fetch_add(1, std::memory_order_relaxed)
                 % g_pool->slots.size();
    return g_pool->slots[idx].get();
}

// Reconnects automatically if the context died (network hiccup, server restart).
template<typename F>
static redisReply* slot_exec(Slot& slot, F&& make_reply) {
    std::lock_guard lk(slot.mu);
    if (!slot.ctx) {
        try { slot.ctx = g_pool->connect_one(); }
        catch (const std::exception& ex) {
            spdlog::warn("{}", ex.what());
            return nullptr;
        }
    }
    redisReply* r = make_reply(slot.ctx);
    if (!r || slot.ctx->err) {
        redisFree(slot.ctx);
        slot.ctx = nullptr;
        if (r) freeReplyObject(r);
        return nullptr;
    }
    return r;
}

} // namespace

void Redis::init(const Config& cfg, int pool_size) {
    if (cfg.redis_url.empty()) {
        spdlog::warn("[redis] REDIS_URL not set - caching disabled");
        return;
    }
    auto pool = std::make_unique<Pool>();
    parse_redis_url(*pool, cfg.redis_url);
    spdlog::info("[redis] {}:{}/{} pool={}", pool->host, pool->port, pool->db, pool_size);

    for (int i = 0; i < pool_size; ++i) {
        auto slot = std::make_unique<Slot>();
        try { slot->ctx = pool->connect_one(); }
        catch (const std::exception& ex) {
            spdlog::warn("[redis] slot {} lazy-connect: {}", i, ex.what());
        }
        pool->slots.push_back(std::move(slot));
    }
    g_pool = std::move(pool);
}

std::optional<std::string> Redis::get(std::string_view key) {
    auto* slot = pick_slot();
    if (!slot) return std::nullopt;

    auto* r = slot_exec(*slot, [&](redisContext* c) {
        return static_cast<redisReply*>(
            redisCommand(c, "GET %b", key.data(), key.size()));
    });
    if (!r) return std::nullopt;

    std::optional<std::string> result;
    if (r->type == REDIS_REPLY_STRING)
        result = std::string(r->str, static_cast<size_t>(r->len));
    freeReplyObject(r);
    return result;
}

void Redis::set(std::string_view key, std::string_view value,
                std::chrono::seconds ttl) {
    auto* slot = pick_slot();
    if (!slot) return;

    auto* r = slot_exec(*slot, [&](redisContext* c) -> redisReply* {
        if (ttl.count() > 0) {
            return static_cast<redisReply*>(
                redisCommand(c, "SETEX %b %lld %b",
                             key.data(), key.size(),
                             static_cast<long long>(ttl.count()),
                             value.data(), value.size()));
        }
        return static_cast<redisReply*>(
            redisCommand(c, "SET %b %b", key.data(), key.size(), value.data(), value.size()));
    });
    if (r) freeReplyObject(r);
}

void Redis::del(std::string_view key) {
    auto* slot = pick_slot();
    if (!slot) return;
    auto* r = slot_exec(*slot, [&](redisContext* c) {
        return static_cast<redisReply*>(
            redisCommand(c, "DEL %b", key.data(), key.size()));
    });
    if (r) freeReplyObject(r);
}

// ---------------------------------------------------------------------------
// List + scan operations (added for the iter-1 ingest queue).
// ---------------------------------------------------------------------------

long long Redis::lpush(std::string_view key, std::string_view value) {
    auto* slot = pick_slot();
    if (!slot) return -1;
    long long n = -1;
    auto* r = slot_exec(*slot, [&](redisContext* c) {
        return static_cast<redisReply*>(
            redisCommand(c, "LPUSH %b %b",
                         key.data(), key.size(),
                         value.data(), value.size()));
    });
    if (r) {
        if (r->type == REDIS_REPLY_INTEGER) n = r->integer;
        freeReplyObject(r);
    }
    return n;
}

std::optional<std::string> Redis::rpop(std::string_view key) {
    auto* slot = pick_slot();
    if (!slot) return std::nullopt;
    auto* r = slot_exec(*slot, [&](redisContext* c) {
        return static_cast<redisReply*>(
            redisCommand(c, "RPOP %b", key.data(), key.size()));
    });
    if (!r) return std::nullopt;
    std::optional<std::string> result;
    if (r->type == REDIS_REPLY_STRING)
        result = std::string(r->str, static_cast<size_t>(r->len));
    freeReplyObject(r);
    return result;
}

std::optional<std::string>
Redis::lmove_right_left(std::string_view src, std::string_view dst) {
    auto* slot = pick_slot();
    if (!slot) return std::nullopt;
    auto* r = slot_exec(*slot, [&](redisContext* c) {
        return static_cast<redisReply*>(
            redisCommand(c, "LMOVE %b %b RIGHT LEFT",
                         src.data(), src.size(),
                         dst.data(), dst.size()));
    });
    if (!r) return std::nullopt;
    std::optional<std::string> result;
    if (r->type == REDIS_REPLY_STRING)
        result = std::string(r->str, static_cast<size_t>(r->len));
    freeReplyObject(r);
    return result;
}

long long Redis::lrem(std::string_view key, long long count,
                       std::string_view value) {
    auto* slot = pick_slot();
    if (!slot) return -1;
    long long removed = -1;
    auto* r = slot_exec(*slot, [&](redisContext* c) {
        return static_cast<redisReply*>(
            redisCommand(c, "LREM %b %lld %b",
                         key.data(), key.size(),
                         count,
                         value.data(), value.size()));
    });
    if (r) {
        if (r->type == REDIS_REPLY_INTEGER) removed = r->integer;
        freeReplyObject(r);
    }
    return removed;
}

std::vector<std::string>
Redis::lrange(std::string_view key, long long start, long long stop) {
    std::vector<std::string> out;
    auto* slot = pick_slot();
    if (!slot) return out;
    auto* r = slot_exec(*slot, [&](redisContext* c) {
        return static_cast<redisReply*>(
            redisCommand(c, "LRANGE %b %lld %lld",
                         key.data(), key.size(), start, stop));
    });
    if (!r) return out;
    if (r->type == REDIS_REPLY_ARRAY) {
        out.reserve(r->elements);
        for (size_t i = 0; i < r->elements; ++i) {
            auto* e = r->element[i];
            if (e && e->type == REDIS_REPLY_STRING)
                out.emplace_back(e->str, static_cast<size_t>(e->len));
        }
    }
    freeReplyObject(r);
    return out;
}

int Redis::exists(std::string_view key) {
    auto* slot = pick_slot();
    if (!slot) return -1;
    int present = -1;
    auto* r = slot_exec(*slot, [&](redisContext* c) {
        return static_cast<redisReply*>(
            redisCommand(c, "EXISTS %b", key.data(), key.size()));
    });
    if (r) {
        if (r->type == REDIS_REPLY_INTEGER)
            present = static_cast<int>(r->integer);
        freeReplyObject(r);
    }
    return present;
}

int Redis::transfer_proc_to_source(std::string_view src,
                                    std::string_view dst,
                                    std::string_view payload)
{
    // Atomic LREM-then-conditional-LPUSH. Only the caller whose LREM
    // observes count > 0 performs the LPUSH; concurrent invocations
    // therefore cannot both push the same payload to `dst`. This is
    // the only safe way to coordinate the LMOVE-window recovery across
    // multiple scheduler instances without holding a Postgres
    // transaction lock through Redis I/O.
    static constexpr const char* kScript =
        "local removed = redis.call('LREM', KEYS[1], 1, ARGV[1])\n"
        "if removed > 0 then\n"
        "  redis.call('LPUSH', KEYS[2], ARGV[1])\n"
        "  return 1\n"
        "else\n"
        "  return 0\n"
        "end\n";

    auto* slot = pick_slot();
    if (!slot) return -1;
    int outcome = -1;
    auto* r = slot_exec(*slot, [&](redisContext* c) {
        return static_cast<redisReply*>(
            redisCommand(c, "EVAL %s 2 %b %b %b",
                         kScript,
                         src.data(),     src.size(),
                         dst.data(),     dst.size(),
                         payload.data(), payload.size()));
    });
    if (r) {
        if (r->type == REDIS_REPLY_INTEGER)
            outcome = static_cast<int>(r->integer);
        freeReplyObject(r);
    }
    return outcome;
}

std::vector<std::string> Redis::scan_keys(std::string_view pattern, size_t limit) {
    std::vector<std::string> keys;
    auto* slot = pick_slot();
    if (!slot) return keys;

    long long cursor = 0;
    do {
        // Use a per-iteration COUNT of 256; Redis treats this as a hint.
        auto* r = slot_exec(*slot, [&](redisContext* c) {
            return static_cast<redisReply*>(
                redisCommand(c, "SCAN %lld MATCH %b COUNT 256",
                             cursor,
                             pattern.data(), pattern.size()));
        });
        if (!r) break;
        if (r->type != REDIS_REPLY_ARRAY || r->elements < 2) {
            freeReplyObject(r);
            break;
        }
        // r->element[0] = next cursor (as string), r->element[1] = array of keys.
        cursor = std::stoll(r->element[0]->str);
        redisReply* arr = r->element[1];
        for (size_t i = 0; i < arr->elements && keys.size() < limit; ++i) {
            redisReply* kr = arr->element[i];
            if (kr->type == REDIS_REPLY_STRING)
                keys.emplace_back(kr->str, static_cast<size_t>(kr->len));
        }
        freeReplyObject(r);
    } while (cursor != 0 && keys.size() < limit);

    return keys;
}

// ---------------------------------------------------------------------------
// Concurrency semaphore (lease-based, crash-safe)
// ---------------------------------------------------------------------------

int Redis::sem_acquire(std::string_view key, int cap, long long lease_ttl_ms,
                       std::string_view token)
{
    // Prune leases that expired at or before now (crashed holders never
    // reclaimed their slot), then admit if under cap. ZADD stores the new
    // lease scored by its expiry so it self-prunes on a future acquire; the
    // key-level PEXPIRE is a backstop so an idle tenant's ZSET is reclaimed.
    //
    // `now` is read from redis.call('TIME') so all app nodes share ONE clock:
    // a skewed client clock cannot prune another node's still-active lease.
    // replicate_commands() makes the (non-deterministic) TIME read safe to
    // follow with writes on Redis 5-6 (no-op on 7+).
    // ARGV: [1]=lease_ttl_ms, [2]=cap, [3]=token.
    static constexpr const char* kScript =
        "redis.replicate_commands()\n"
        "local t = redis.call('TIME')\n"
        "local now = tonumber(t[1]) * 1000 + math.floor(tonumber(t[2]) / 1000)\n"
        "redis.call('ZREMRANGEBYSCORE', KEYS[1], '-inf', now)\n"
        "if redis.call('ZCARD', KEYS[1]) >= tonumber(ARGV[2]) then return 0 end\n"
        "redis.call('ZADD', KEYS[1], now + tonumber(ARGV[1]), ARGV[3])\n"
        "redis.call('PEXPIRE', KEYS[1], tonumber(ARGV[1]) * 2)\n"
        "return 1\n";

    auto* slot = pick_slot();
    if (!slot) return -1;
    const std::string ttl_s = std::to_string(lease_ttl_ms);
    const std::string cap_s = std::to_string(cap);
    int outcome = -1;
    auto* r = slot_exec(*slot, [&](redisContext* c) {
        return static_cast<redisReply*>(
            redisCommand(c, "EVAL %s 1 %b %s %s %b",
                         kScript,
                         key.data(),   key.size(),
                         ttl_s.c_str(), cap_s.c_str(),
                         token.data(), token.size()));
    });
    if (r) {
        if (r->type == REDIS_REPLY_INTEGER) outcome = static_cast<int>(r->integer);
        freeReplyObject(r);
    }
    return outcome;
}

void Redis::sem_release(std::string_view key, std::string_view token)
{
    auto* slot = pick_slot();
    if (!slot) return;
    auto* r = slot_exec(*slot, [&](redisContext* c) {
        return static_cast<redisReply*>(
            redisCommand(c, "ZREM %b %b",
                         key.data(),   key.size(),
                         token.data(), token.size()));
    });
    if (r) freeReplyObject(r);
}

// ---------------------------------------------------------------------------
// Token-bucket rate limiter
// ---------------------------------------------------------------------------

int Redis::token_bucket_take(std::string_view key, double refill_per_sec,
                             int burst, int cost)
{
    // Standard token bucket: refill (bounded by burst) based on elapsed time,
    // then take `cost` if available. State is a hash {t: tokens, ts: last_ms}.
    // `now` and the stored `ts` both come from redis.call('TIME'), so every app
    // node meters against ONE clock; skewed client clocks cannot repeatedly
    // over-refill. replicate_commands() keeps the TIME-then-write script valid
    // on Redis 5-6 (no-op on 7+). ARGV: [1]=rate, [2]=burst, [3]=cost.
    static constexpr const char* kScript =
        "redis.replicate_commands()\n"
        "local t = redis.call('TIME')\n"
        "local now = tonumber(t[1]) * 1000 + math.floor(tonumber(t[2]) / 1000)\n"
        "local rate = tonumber(ARGV[1])\n"
        "local burst = tonumber(ARGV[2])\n"
        "local cost = tonumber(ARGV[3])\n"
        "local d = redis.call('HMGET', KEYS[1], 't', 'ts')\n"
        "local tokens = tonumber(d[1])\n"
        "local ts = tonumber(d[2])\n"
        "if tokens == nil then tokens = burst; ts = now end\n"
        "local delta = now - ts\n"
        "if delta < 0 then delta = 0 end\n"
        "tokens = math.min(burst, tokens + (delta / 1000.0) * rate)\n"
        "local allowed = 0\n"
        "if tokens >= cost then tokens = tokens - cost; allowed = 1 end\n"
        "redis.call('HSET', KEYS[1], 't', tokens, 'ts', now)\n"
        "redis.call('PEXPIRE', KEYS[1], math.ceil((burst / rate) * 1000) + 1000)\n"
        "return allowed\n";

    auto* slot = pick_slot();
    if (!slot) return -1;
    const std::string rate_s  = std::to_string(refill_per_sec);
    const std::string burst_s = std::to_string(burst);
    const std::string cost_s  = std::to_string(cost);
    int outcome = -1;
    auto* r = slot_exec(*slot, [&](redisContext* c) {
        return static_cast<redisReply*>(
            redisCommand(c, "EVAL %s 1 %b %s %s %s",
                         kScript,
                         key.data(), key.size(),
                         rate_s.c_str(), burst_s.c_str(), cost_s.c_str()));
    });
    if (r) {
        if (r->type == REDIS_REPLY_INTEGER) outcome = static_cast<int>(r->integer);
        freeReplyObject(r);
    }
    return outcome;
}

} // namespace wikore
