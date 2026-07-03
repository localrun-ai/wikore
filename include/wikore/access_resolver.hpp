#pragma once
#include "wikore/domain/types.hpp"
#include "wikore/adapters/postgres/deadline_exec.hpp"
#include <drogon/drogon.h>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace wikore {

// ---------------------------------------------------------------------------
// AccessResolver (Iteration 2)
//
// Resolves a (tenant, principal, query-scope) to the set of org_unit_ids the
// principal can read - the exact `reader_scope` the retrieval gate consumes
// (G1; docs/iteration_2_design.md section 0). This is the read-path resolver;
// membership/grant CRUD stays in AccessService.
//
// The resolution is a SINGLE SQL statement, so its output is one Read
// Committed snapshot and cannot tear under a concurrent move_org_unit
// (scenario E / section-5 item 3). It mirrors the proven
// AccessService::effective_read_orgs scope logic and additionally captures, in
// the SAME snapshot, the companies.acl_epoch and users.scope_epoch the scope
// was resolved under. Those stamps let the section-2.6 Redis cache validate an
// entry against the live epochs: a stale entry is detectable (and re-resolved)
// even if its lr:eff invalidation was lost.
//
// TODO(iter2-consolidation): AccessService::effective_read_orgs duplicates the
// scope subquery below. Once the cache layer lands, fold effective_read_orgs
// onto this resolver so the security-critical scope SQL has one home.
//
// Returns the domain `AccessScope` (wikore/domain/types.hpp): org_unit_ids is
// the gate's reader_scope; access_epoch (companies.acl_epoch) and scope_epoch
// (users.scope_epoch) are the stamps the section-2.6 cache validates against.
// cache_until is left default by this (uncached) adapter; the cache layer
// computes the TTL clamp (V008) when it lands.
// ---------------------------------------------------------------------------

class AccessResolverPort {
public:
    virtual ~AccessResolverPort() = default;

    // deadline bounds the DB work to the remaining request budget (Postgres
    // statement_timeout); the default max() means unbounded.
    virtual drogon::Task<Result<AccessScope>>
    resolve(std::string_view company_id,
            std::string_view user_id,
            std::string_view scope_org_unit_id,
            postgres::Deadline deadline = postgres::no_deadline()) const = 0;
};

class PostgresAccessResolver : public AccessResolverPort {
public:
    explicit PostgresAccessResolver(drogon::orm::DbClientPtr db)
        : db_(std::move(db)) {}

    drogon::Task<Result<AccessScope>>
    resolve(std::string_view company_id,
            std::string_view user_id,
            std::string_view scope_org_unit_id,
            postgres::Deadline deadline = postgres::no_deadline()) const override;

private:
    drogon::orm::DbClientPtr db_;
};

// ---------------------------------------------------------------------------
// CachedAccessResolver (section 2.6)
//
// Redis lr:eff cache in front of an inner resolver. A cache hit is served only
// when the entry is unexpired (its cache_until) AND its stamped epochs match
// the LIVE (companies.acl_epoch, users.scope_epoch). The epoch check is the
// crash-safe correctness path: a stale entry is detected even if its lr:eff
// invalidation (DEL / reverse-index evict) was lost. On miss or stale it
// delegates to `inner` and writes the entry with a TTL clamped to cache_until.
//
// The live-epoch read is a single cheap two-int Postgres lookup, far cheaper
// than the inner scope resolution (closure + membership joins) it lets us
// skip. (Mirroring the company epoch into Redis to drop even that lookup, and
// piggybacking scope_epoch on the identity cache, are the further section-2.6
// optimizations; not required for correctness.)
// ---------------------------------------------------------------------------
class CachedAccessResolver : public AccessResolverPort {
public:
    CachedAccessResolver(std::shared_ptr<AccessResolverPort> inner,
                         drogon::orm::DbClientPtr            db)
        : inner_(std::move(inner)), db_(std::move(db)) {}

    drogon::Task<Result<AccessScope>>
    resolve(std::string_view company_id,
            std::string_view user_id,
            std::string_view scope_org_unit_id,
            postgres::Deadline deadline = postgres::no_deadline()) const override;

private:
    std::shared_ptr<AccessResolverPort> inner_;
    drogon::orm::DbClientPtr            db_;
};

} // namespace wikore
