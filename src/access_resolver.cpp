#include "wikore/access_resolver.hpp"
#include "wikore/redis.hpp"
#include "wikore/adapters/postgres/deadline_exec.hpp"
#include <drogon/drogon.h>
#include <glaze/glaze.hpp>
#include <algorithm>
#include <chrono>

namespace wikore {

drogon::Task<Result<AccessScope>>
PostgresAccessResolver::resolve(std::string_view company_id,
                                std::string_view user_id,
                                std::string_view scope_org_unit_id,
                                postgres::Deadline deadline) const
{
    // SINGLE statement: the scope set AND (acl_epoch, scope_epoch) in one Read
    // Committed snapshot. The scope CTE mirrors AccessService::
    // effective_read_orgs (proven + tested). epochs is LEFT JOINed so the
    // epochs are returned even when the scope is empty; org_unit_id is NULL on
    // that single epoch-only row. Zero rows means the (company, user) pair does
    // not exist -> NotFound (tenant isolation: the join requires u.company_id = $1).
    constexpr auto kSql = R"(
        WITH scope AS (
            SELECT DISTINCT sub.descendant_id::text AS org_unit_id
            FROM (
                SELECT m.org_unit_id, m.applies_to
                FROM   memberships m
                WHERE  m.company_id = $1::uuid AND m.user_id = $2::uuid
                  AND  (m.expires_at IS NULL OR m.expires_at > now())
                UNION
                SELECT m.org_unit_id, m.applies_to
                FROM   group_members gm
                JOIN   memberships m
                    ON m.company_id = gm.company_id AND m.group_id = gm.group_id
                WHERE  gm.company_id = $1::uuid AND gm.user_id = $2::uuid
                  AND  (m.expires_at IS NULL OR m.expires_at > now())
            ) base
            JOIN org_unit_closure sub
                ON sub.company_id  = $1::uuid
               AND sub.ancestor_id = base.org_unit_id
               AND (base.applies_to = 'self_and_descendants' OR sub.depth = 0)
            JOIN org_unit_closure scope
                ON scope.company_id    = $1::uuid
               AND scope.ancestor_id   = $3::uuid
               AND scope.descendant_id = sub.descendant_id
        ),
        epochs AS (
            SELECT c.acl_epoch, u.scope_epoch
            FROM   companies c
            JOIN   users u ON u.company_id = c.id
            WHERE  c.id = $1::uuid AND u.id = $2::uuid
        ),
        -- Earliest expiry among the memberships that CONTRIBUTE to the scope.
        -- The lr:eff TTL is clamped to this (V008) so a membership lapsing
        -- mid-window cannot keep granting via a cached scope. Reader scope is
        -- membership-only, so resource_grant expiry is irrelevant here.
        min_expiry AS (
            SELECT min(e) AS min_expiry FROM (
                SELECT m.expires_at AS e
                FROM   memberships m
                WHERE  m.company_id = $1::uuid AND m.user_id = $2::uuid
                  AND  (m.expires_at IS NULL OR m.expires_at > now())
                UNION ALL
                SELECT m.expires_at
                FROM   group_members gm
                JOIN   memberships m
                    ON m.company_id = gm.company_id AND m.group_id = gm.group_id
                WHERE  gm.company_id = $1::uuid AND gm.user_id = $2::uuid
                  AND  (m.expires_at IS NULL OR m.expires_at > now())
            ) x
        )
        SELECT e.acl_epoch    AS acl_epoch,
               e.scope_epoch  AS scope_epoch,
               -- FLOOR to the millisecond, never round: a rounded-up expiry
               -- would extend the cached scope's TTL past the real expiry and
               -- briefly keep an expired membership authorized.
               floor(extract(epoch FROM x.min_expiry) * 1000)::bigint AS min_expiry_ms,
               s.org_unit_id  AS org_unit_id
        FROM   epochs e
        CROSS JOIN min_expiry x
        LEFT JOIN scope s ON true
    )";

    try {
        auto rows = co_await postgres::exec_until(
            db_, deadline, kSql, std::string(company_id), std::string(user_id),
            std::string(scope_org_unit_id));

        if (rows.empty())
            co_return std::unexpected(Error::not_found(
                "access_resolver: principal not found in tenant"));

        AccessScope scope;
        scope.access_epoch = rows[0]["acl_epoch"].as<std::int64_t>();
        scope.scope_epoch  = rows[0]["scope_epoch"].as<std::int64_t>();

        // cache_until = min(now + default TTL, earliest contributing expiry).
        using namespace std::chrono;
        constexpr auto kDefaultTtl = minutes(5);     // V008 default
        auto cache_until = system_clock::now() + kDefaultTtl;
        if (!rows[0]["min_expiry_ms"].isNull()) {
            const auto exp = system_clock::time_point(
                milliseconds(rows[0]["min_expiry_ms"].as<long long>()));
            cache_until = std::min(cache_until, exp);   // floored: never past expiry
        }
        scope.cache_until = cache_until;

        scope.org_unit_ids.reserve(rows.size());
        for (const auto& r : rows)
            if (!r["org_unit_id"].isNull())
                scope.org_unit_ids.push_back(r["org_unit_id"].as<std::string>());
        co_return scope;
    } catch (const drogon::orm::DrogonDbException& ex) {
        co_return std::unexpected(Error::database_error(
            std::string("access_resolver: ") + ex.base().what()));
    }
}

// ---------------------------------------------------------------------------
// CachedAccessResolver
// ---------------------------------------------------------------------------

namespace {

// On-disk (Redis) shape for a cached scope. Explicit glz::meta so the JSON
// keys are stable across builds (it crosses the process boundary).
struct CachedScopeDto {
    std::vector<std::string> org_unit_ids;
    std::int64_t             access_epoch   = 0;
    std::int64_t             scope_epoch    = 0;
    long long                cache_until_ms = 0;
};

} // namespace

} // namespace wikore

template <>
struct glz::meta<wikore::CachedScopeDto> {
    using T = wikore::CachedScopeDto;
    static constexpr auto value = glz::object(
        "org_unit_ids",   &T::org_unit_ids,
        "access_epoch",   &T::access_epoch,
        "scope_epoch",    &T::scope_epoch,
        "cache_until_ms", &T::cache_until_ms);
};

namespace wikore {

drogon::Task<Result<AccessScope>>
CachedAccessResolver::resolve(std::string_view company_id,
                              std::string_view user_id,
                              std::string_view scope_org_unit_id,
                              postgres::Deadline deadline) const
{
    using namespace std::chrono;
    const std::string key = "lr:eff:" + std::string(company_id) + ":"
                          + std::string(user_id) + ":"
                          + std::string(scope_org_unit_id);
    const auto now    = system_clock::now();
    const auto now_ms = duration_cast<milliseconds>(now.time_since_epoch()).count();

    // --- fast path: a fresh, epoch-valid cache entry ---
    if (auto cached = Redis::get(key)) {
        CachedScopeDto dto;
        if (!glz::read_json(dto, *cached) && now_ms < dto.cache_until_ms) {
            // Validate the stamp against the LIVE epochs (cheap: two ints).
            // Any DB hiccup here just falls through to a full re-resolve.
            try {
                auto ep = co_await postgres::exec_until(
                    db_, deadline,
                    "SELECT c.acl_epoch, u.scope_epoch "
                    "FROM companies c JOIN users u ON u.company_id = c.id "
                    "WHERE c.id = $1::uuid AND u.id = $2::uuid",
                    std::string(company_id), std::string(user_id));
                if (!ep.empty()
                    && ep[0]["acl_epoch"].as<std::int64_t>()   == dto.access_epoch
                    && ep[0]["scope_epoch"].as<std::int64_t>() == dto.scope_epoch)
                {
                    AccessScope s;
                    s.org_unit_ids = std::move(dto.org_unit_ids);
                    s.access_epoch = dto.access_epoch;
                    s.scope_epoch  = dto.scope_epoch;
                    s.cache_until  = system_clock::time_point(milliseconds(dto.cache_until_ms));
                    co_return s;
                }
            } catch (const drogon::orm::DrogonDbException&) {
                // fall through to re-resolve
            }
        }
    }

    // --- miss / stale / unparseable: authoritative resolve, then cache ---
    auto r = co_await inner_->resolve(company_id, user_id, scope_org_unit_id, deadline);
    if (!r)
        co_return r;   // never cache errors (e.g. NotFound)

    const auto cu_ms = duration_cast<milliseconds>(r->cache_until.time_since_epoch()).count();
    CachedScopeDto dto{r->org_unit_ids, r->access_epoch, r->scope_epoch, cu_ms};
    std::string json;
    (void)glz::write_json(dto, json);
    const auto ttl = duration_cast<seconds>(r->cache_until - now);
    if (!json.empty() && ttl.count() > 0)
        Redis::set(key, json, ttl);

    co_return r;
}

} // namespace wikore
