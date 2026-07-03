#include <catch2/catch_test_macros.hpp>
#include "wikore/access_resolver.hpp"
#include "wikore/db.hpp"
#include <drogon/drogon.h>
#include <drogon/utils/coroutine.h>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>

// Integration tests for PostgresAccessResolver (Iteration 2 read-path resolver).
// Skip when DATABASE_URL is unset. The scope-set correctness is covered broadly
// by the G1 property test and test_access; here we focus on the resolver's
// contract: the scope set, the (acl_epoch, scope_epoch) stamp, and that the
// stamp reflects the V032 bump triggers (the basis for cache validation).

namespace {

constexpr auto CO = "a5e50000-0000-0000-0000-0000000000a1"; // "access resolver co"

bool db_available()    { return std::getenv("DATABASE_URL") != nullptr; }
bool redis_available() { return std::getenv("REDIS_URL")    != nullptr; }

// Inner resolver that counts calls and returns a canned scope, so we can
// observe whether the cache served a hit (inner not called) or re-resolved.
struct SpyResolver : wikore::AccessResolverPort {
    mutable int          calls = 0;
    wikore::AccessScope  canned;
    drogon::Task<wikore::Result<wikore::AccessScope>>
    resolve(std::string_view, std::string_view, std::string_view,
            wikore::postgres::Deadline) const override {
        ++calls;
        co_return canned;
    }
};

template<typename... Args>
drogon::orm::Result exec_sync(drogon::orm::DbClientPtr db, std::string sql, Args... args)
{
    return drogon::sync_wait(
        [db, sql = std::move(sql), ...args = std::move(args)]()
        -> drogon::Task<drogon::orm::Result> {
            co_return co_await db->execSqlCoro(sql, args...);
        }());
}

struct Fixture {
    std::string root, ou, user;
};

Fixture seed(drogon::orm::DbClientPtr db)
{
    exec_sync(db, "DELETE FROM companies WHERE id=$1::uuid", std::string(CO));
    exec_sync(db, "INSERT INTO companies (id,name,slug) VALUES ($1::uuid,'AR','ar')",
              std::string(CO));
    Fixture f;
    f.root = std::string(exec_sync(db,
        "SELECT id FROM org_units WHERE company_id=$1::uuid AND type='root'",
        std::string(CO))[0]["id"].c_str());
    f.ou = std::string(exec_sync(db,
        "INSERT INTO org_units (company_id,parent_id,type,slug,name) "
        "VALUES ($1::uuid,$2::uuid,'department','eng','Eng') RETURNING id",
        std::string(CO), f.root)[0]["id"].c_str());
    f.user = std::string(exec_sync(db,
        "INSERT INTO users (company_id,external_sub,email) "
        "VALUES ($1::uuid,'subAR','ar@test') RETURNING id",
        std::string(CO))[0]["id"].c_str());
    return f;
}

bool contains(const std::vector<std::string>& v, const std::string& s)
{
    return std::find(v.begin(), v.end(), s) != v.end();
}

} // namespace

TEST_CASE("AccessResolver: resolves scope and stamps epochs", "[integration][access_resolver]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    auto f  = seed(db);
    wikore::PostgresAccessResolver resolver(db);

    // Member of eng (self_and_descendants).
    exec_sync(db,
        "INSERT INTO memberships (company_id,user_id,org_unit_id,role,applies_to) "
        "VALUES ($1::uuid,$2::uuid,$3::uuid,'viewer','self_and_descendants')",
        std::string(CO), f.user, f.ou);

    auto r = drogon::sync_wait(resolver.resolve(CO, f.user, f.root));
    REQUIRE(r.has_value());
    CHECK(contains(r->org_unit_ids, f.ou));
    CHECK(r->access_epoch >= 1);  // companies.acl_epoch defaults to 1 (V032)
    CHECK(r->scope_epoch  >= 1);  // users.scope_epoch defaults to 1, +1 per membership
    CHECK_FALSE(r->org_unit_ids.empty());
}

TEST_CASE("AccessResolver: no memberships yields empty scope but valid epochs (not an error)",
          "[integration][access_resolver]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    auto f  = seed(db);                 // user created, no memberships
    wikore::PostgresAccessResolver resolver(db);

    auto r = drogon::sync_wait(resolver.resolve(CO, f.user, f.root));
    REQUIRE(r.has_value());
    CHECK(r->org_unit_ids.empty());     // fail-closed: no scope
    CHECK(r->access_epoch >= 1);        // epochs still stamped (cache can validate)
    CHECK(r->scope_epoch  >= 1);
}

TEST_CASE("AccessResolver: unknown principal is NotFound", "[integration][access_resolver]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    auto f  = seed(db);
    wikore::PostgresAccessResolver resolver(db);

    auto r = drogon::sync_wait(resolver.resolve(
        CO, "deadbeef-0000-0000-0000-000000000000", f.root));
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().kind == wikore::Error::Kind::NotFound);
}

TEST_CASE("exec_until: a query past the deadline is cancelled quickly, not run to completion",
          "[integration][access_resolver]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();

    // A 2s query with only a 100ms budget must be cancelled by the server's
    // statement_timeout in ~100ms, not run the full 2s. This proves the
    // deadline actually bounds DB awaits (Postgres SQLSTATE 57014).
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(100);
    const auto t0 = std::chrono::steady_clock::now();
    bool threw = false;
    try {
        drogon::sync_wait(wikore::postgres::exec_until(db, deadline,
                                                       "SELECT pg_sleep(2)"));
    } catch (const drogon::orm::DrogonDbException&) {
        threw = true;   // query_canceled
    }
    const auto elapsed = std::chrono::steady_clock::now() - t0;

    CHECK(threw);
    CHECK(elapsed < std::chrono::seconds(1));   // far below the 2s sleep

    // The unbounded sentinel still runs normally (no statement_timeout).
    auto ok = drogon::sync_wait(wikore::postgres::exec_until(
        db, wikore::postgres::no_deadline(), "SELECT 1 AS x"));
    CHECK(ok[0]["x"].as<int>() == 1);
}

TEST_CASE("AccessResolver: scope_epoch stamp advances after a membership change (V032 trigger)",
          "[integration][access_resolver]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    auto f  = seed(db);
    wikore::PostgresAccessResolver resolver(db);

    auto r0 = drogon::sync_wait(resolver.resolve(CO, f.user, f.root));
    REQUIRE(r0.has_value());
    const auto e0 = r0->scope_epoch;

    exec_sync(db,
        "INSERT INTO memberships (company_id,user_id,org_unit_id,role,applies_to) "
        "VALUES ($1::uuid,$2::uuid,$3::uuid,'viewer','self_only')",
        std::string(CO), f.user, f.ou);

    auto r1 = drogon::sync_wait(resolver.resolve(CO, f.user, f.root));
    REQUIRE(r1.has_value());
    INFO("scope_epoch e0=" << e0 << " e1=" << r1->scope_epoch);
    CHECK(r1->scope_epoch > e0);   // a stale cache stamped with e0 is now detectable
}

TEST_CASE("AccessResolver: acl_epoch stamp advances after an org structural change (V032 trigger)",
          "[integration][access_resolver]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    auto f  = seed(db);
    wikore::PostgresAccessResolver resolver(db);

    auto r0 = drogon::sync_wait(resolver.resolve(CO, f.user, f.root));
    REQUIRE(r0.has_value());
    const auto a0 = r0->access_epoch;

    // Org-unit create is a structural change -> companies.acl_epoch bumps (V032).
    exec_sync(db,
        "INSERT INTO org_units (company_id,parent_id,type,slug,name) "
        "VALUES ($1::uuid,$2::uuid,'team','t2','T2')",
        std::string(CO), f.root);

    auto r1 = drogon::sync_wait(resolver.resolve(CO, f.user, f.root));
    REQUIRE(r1.has_value());
    INFO("acl_epoch a0=" << a0 << " a1=" << r1->access_epoch);
    CHECK(r1->access_epoch > a0);
}

TEST_CASE("Resolver: cache_until defaults to ~5min and clamps to earliest membership expiry",
          "[integration][access_resolver]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    using namespace std::chrono;
    auto db = wikore::Db::get();
    auto f  = seed(db);
    wikore::PostgresAccessResolver resolver(db);

    // Non-expiring membership -> default ~5 minutes.
    exec_sync(db,
        "INSERT INTO memberships (company_id,user_id,org_unit_id,role,applies_to) "
        "VALUES ($1::uuid,$2::uuid,$3::uuid,'viewer','self_only')",
        std::string(CO), f.user, f.ou);
    auto r1   = drogon::sync_wait(resolver.resolve(CO, f.user, f.root));
    auto now1 = system_clock::now();
    REQUIRE(r1.has_value());
    CHECK(r1->cache_until >  now1 + minutes(4));
    CHECK(r1->cache_until <= now1 + minutes(5) + seconds(5));

    // Add a membership expiring in 30s -> cache_until clamps well under 5min.
    exec_sync(db,
        "INSERT INTO memberships (company_id,user_id,org_unit_id,role,applies_to,expires_at) "
        "VALUES ($1::uuid,$2::uuid,$3::uuid,'viewer','self_and_descendants', now()+interval '30 seconds')",
        std::string(CO), f.user, f.root);
    auto r2   = drogon::sync_wait(resolver.resolve(CO, f.user, f.root));
    auto now2 = system_clock::now();
    REQUIRE(r2.has_value());
    CHECK(r2->cache_until <= now2 + seconds(60));
    CHECK(r2->cache_until >  now2);
}

TEST_CASE("AccessResolver: cache_until floors a fractional expiry, never rounds past it (P2)",
          "[integration][access_resolver]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    using namespace std::chrono;
    auto db = wikore::Db::get();
    auto f  = seed(db);
    // Membership expiring at a whole second + 600ms: extract(epoch)::bigint
    // would round this UP to the next second, extending cache_until past it.
    exec_sync(db,
        "INSERT INTO memberships (company_id,user_id,org_unit_id,role,applies_to,expires_at) "
        "VALUES ($1::uuid,$2::uuid,$3::uuid,'viewer','self_only', "
        "        date_trunc('second', now()) + interval '45.6 seconds')",
        std::string(CO), f.user, f.ou);

    wikore::PostgresAccessResolver resolver(db);
    auto r = drogon::sync_wait(resolver.resolve(CO, f.user, f.root));
    REQUIRE(r.has_value());

    const auto exp_ms = std::stoll(std::string(exec_sync(db,
        "SELECT floor(extract(epoch FROM expires_at)*1000)::bigint::text AS e "
        "FROM memberships WHERE company_id=$1::uuid AND user_id=$2::uuid "
        "  AND expires_at IS NOT NULL ORDER BY expires_at LIMIT 1",
        std::string(CO), f.user)[0]["e"].c_str()));
    const auto cu_ms = duration_cast<milliseconds>(r->cache_until.time_since_epoch()).count();
    INFO("cache_until_ms=" << cu_ms << " expiry_ms=" << exp_ms);
    CHECK(cu_ms <= exp_ms);   // never outlives the real expiry (fails under rounding)
}

TEST_CASE("CachedAccessResolver: epoch-valid hit is served from cache; an epoch bump re-resolves",
          "[integration][access_resolver]")
{
    if (!db_available() || !redis_available())
        SKIP("DATABASE_URL or REDIS_URL not set");
    using namespace std::chrono;
    auto db = wikore::Db::get();
    auto f  = seed(db);

    // Stamp the canned scope with the LIVE epochs so the cache validation passes.
    auto ep = exec_sync(db,
        "SELECT c.acl_epoch, u.scope_epoch FROM companies c "
        "JOIN users u ON u.company_id=c.id WHERE c.id=$1::uuid AND u.id=$2::uuid",
        std::string(CO), f.user);
    auto spy = std::make_shared<SpyResolver>();
    spy->canned.access_epoch = ep[0]["acl_epoch"].as<std::int64_t>();
    spy->canned.scope_epoch  = ep[0]["scope_epoch"].as<std::int64_t>();
    spy->canned.cache_until  = system_clock::now() + minutes(5);
    spy->canned.org_unit_ids = {f.ou};

    wikore::CachedAccessResolver cached(spy, db);

    auto r1 = drogon::sync_wait(cached.resolve(CO, f.user, f.root));
    REQUIRE(r1.has_value());
    CHECK(spy->calls == 1);                       // miss -> inner resolved
    CHECK(contains(r1->org_unit_ids, f.ou));

    auto r2 = drogon::sync_wait(cached.resolve(CO, f.user, f.root));
    REQUIRE(r2.has_value());
    CHECK(spy->calls == 1);                       // HIT -> inner NOT called again
    CHECK(contains(r2->org_unit_ids, f.ou));

    // Bump users.scope_epoch via a membership change (V032 trigger): the stamp
    // is now stale and the next resolve must fall through to inner.
    exec_sync(db,
        "INSERT INTO memberships (company_id,user_id,org_unit_id,role,applies_to) "
        "VALUES ($1::uuid,$2::uuid,$3::uuid,'viewer','self_only')",
        std::string(CO), f.user, f.ou);

    auto r3 = drogon::sync_wait(cached.resolve(CO, f.user, f.root));
    REQUIRE(r3.has_value());
    INFO("spy calls after epoch bump=" << spy->calls);
    CHECK(spy->calls == 2);                       // stale epoch -> re-resolve
}

TEST_CASE("V032: reassigning a membership bumps BOTH the old and new principal (P1)",
          "[integration][access_resolver]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    auto f  = seed(db);
    const auto userB = std::string(exec_sync(db,
        "INSERT INTO users (company_id,external_sub,email) "
        "VALUES ($1::uuid,'subB','b@ar.test') RETURNING id", std::string(CO))[0]["id"].c_str());
    exec_sync(db,
        "INSERT INTO memberships (company_id,user_id,org_unit_id,role,applies_to) "
        "VALUES ($1::uuid,$2::uuid,$3::uuid,'viewer','self_only')",
        std::string(CO), f.user, f.ou);

    auto epoch = [&](const std::string& u) {
        return std::stoll(std::string(exec_sync(db,
            "SELECT scope_epoch::text AS e FROM users WHERE id=$1::uuid", u)[0]["e"].c_str()));
    };
    const auto a0 = epoch(f.user);
    const auto b0 = epoch(userB);

    // Reassign the membership from f.user to userB (an in-place UPDATE).
    exec_sync(db,
        "UPDATE memberships SET user_id=$1::uuid WHERE company_id=$2::uuid AND user_id=$3::uuid",
        userB, std::string(CO), f.user);

    CHECK(epoch(f.user) > a0);   // OLD principal invalidated (the fix)
    CHECK(epoch(userB) > b0);    // NEW principal invalidated
}

TEST_CASE("V032: narrowing a resource_grant resyncs the documents it no longer covers (P2)",
          "[integration][access_resolver]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    auto f  = seed(db);
    // tree: root -> A -> B; a document owned by B.
    const auto A = std::string(exec_sync(db,
        "INSERT INTO org_units (company_id,parent_id,type,slug,name) "
        "VALUES ($1::uuid,$2::uuid,'department','a2','A2') RETURNING id",
        std::string(CO), f.root)[0]["id"].c_str());
    const auto B = std::string(exec_sync(db,
        "INSERT INTO org_units (company_id,parent_id,type,slug,name) "
        "VALUES ($1::uuid,$2::uuid,'team','b2','B2') RETURNING id",
        std::string(CO), A)[0]["id"].c_str());
    const auto doc = std::string(exec_sync(db,
        "INSERT INTO documents (company_id,owner_org_unit_id,filename) "
        "VALUES ($1::uuid,$2::uuid,'d.txt') RETURNING id",
        std::string(CO), B)[0]["id"].c_str());
    auto docver = [&] {
        return std::stoll(std::string(exec_sync(db,
            "SELECT acl_version::text AS v FROM documents WHERE id=$1::uuid", doc)[0]["v"].c_str()));
    };

    // Grant on A, self_and_descendants -> covers B's subtree, including the doc.
    exec_sync(db,
        "INSERT INTO resource_grants (company_id,resource_type,resource_id,resource_applies_to,"
        "principal_type,principal_id,principal_applies_to,permission) "
        "VALUES ($1::uuid,'org_unit',$2::uuid,'self_and_descendants','org_unit',$2::uuid,'self_only','read')",
        std::string(CO), A);
    const auto v_after_insert = docver();   // bumped by INSERT (doc owner in A subtree)

    // Narrow to self_only: the doc (owned by B) is no longer covered. The OLD
    // scope must still reprocess it, or its payload would stay stale forever.
    exec_sync(db,
        "UPDATE resource_grants SET resource_applies_to='self_only' "
        "WHERE company_id=$1::uuid AND resource_type='org_unit' AND resource_id=$2::uuid",
        std::string(CO), A);
    CHECK(docver() > v_after_insert);       // OLD scope reprocessed the dropped doc
}

TEST_CASE("V032: a cross-tenant grant move still resyncs the OLD tenant's documents (P2)",
          "[integration][access_resolver]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    auto f  = seed(db);                                  // company CO, f.root
    constexpr auto CO2 = "a5e50000-0000-0000-0000-0000000000c2";
    exec_sync(db, "DELETE FROM companies WHERE id=$1::uuid", std::string(CO2));
    exec_sync(db, "INSERT INTO companies (id,name,slug) VALUES ($1::uuid,'AR2','ar2')",
              std::string(CO2));
    const auto root2 = std::string(exec_sync(db,
        "SELECT id FROM org_units WHERE company_id=$1::uuid AND type='root'",
        std::string(CO2))[0]["id"].c_str());
    const auto ouY = std::string(exec_sync(db,
        "INSERT INTO org_units (company_id,parent_id,type,slug,name) "
        "VALUES ($1::uuid,$2::uuid,'team','y','Y') RETURNING id",
        std::string(CO2), root2)[0]["id"].c_str());

    // company1: an OU, a document owned by it, and a grant on that OU.
    const auto ouX = std::string(exec_sync(db,
        "INSERT INTO org_units (company_id,parent_id,type,slug,name) "
        "VALUES ($1::uuid,$2::uuid,'team','x','X') RETURNING id",
        std::string(CO), f.root)[0]["id"].c_str());
    const auto doc = std::string(exec_sync(db,
        "INSERT INTO documents (company_id,owner_org_unit_id,filename) "
        "VALUES ($1::uuid,$2::uuid,'x.txt') RETURNING id",
        std::string(CO), ouX)[0]["id"].c_str());
    exec_sync(db,
        "INSERT INTO resource_grants (company_id,resource_type,resource_id,resource_applies_to,"
        "principal_type,principal_id,principal_applies_to,permission) "
        "VALUES ($1::uuid,'org_unit',$2::uuid,'self_only','org_unit',$2::uuid,'self_only','read')",
        std::string(CO), ouX);
    auto docver = [&] {
        return std::stoll(std::string(exec_sync(db,
            "SELECT acl_version::text AS v FROM documents WHERE id=$1::uuid", doc)[0]["v"].c_str()));
    };
    const auto v0 = docver();   // bumped by the INSERT

    // Move the grant to company2 (V009 permits it: the NEW actors are valid in
    // CO2). The OLD tenant's document must still be resynced under CO.
    exec_sync(db,
        "UPDATE resource_grants SET company_id=$1::uuid, resource_id=$2::uuid, principal_id=$2::uuid "
        "WHERE company_id=$3::uuid AND resource_type='org_unit' AND resource_id=$4::uuid",
        std::string(CO2), ouY, std::string(CO), ouX);
    CHECK(docver() > v0);       // OLD-tenant doc bumped under its own company_id

    exec_sync(db, "DELETE FROM companies WHERE id=$1::uuid", std::string(CO2));
}

TEST_CASE("V032: a same-tenant grant UPDATE bumps each document once, not twice (P2)",
          "[integration][access_resolver]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    auto f  = seed(db);
    const auto ouX = std::string(exec_sync(db,
        "INSERT INTO org_units (company_id,parent_id,type,slug,name) "
        "VALUES ($1::uuid,$2::uuid,'team','x3','X3') RETURNING id",
        std::string(CO), f.root)[0]["id"].c_str());
    const auto doc = std::string(exec_sync(db,
        "INSERT INTO documents (company_id,owner_org_unit_id,filename) "
        "VALUES ($1::uuid,$2::uuid,'x3.txt') RETURNING id",
        std::string(CO), ouX)[0]["id"].c_str());
    exec_sync(db,
        "INSERT INTO resource_grants (company_id,resource_type,resource_id,resource_applies_to,"
        "principal_type,principal_id,principal_applies_to,permission) "
        "VALUES ($1::uuid,'org_unit',$2::uuid,'self_only','org_unit',$2::uuid,'self_only','read')",
        std::string(CO), ouX);

    auto docver = [&] {
        return std::stoll(std::string(exec_sync(db,
            "SELECT acl_version::text AS v FROM documents WHERE id=$1::uuid", doc)[0]["v"].c_str()));
    };
    auto events = [&] {
        return std::stoll(std::string(exec_sync(db,
            "SELECT count(*)::text AS n FROM outbox_events "
            "WHERE aggregate_id=$1::uuid AND job_type='qdrant_resync_chunk_acl'",
            doc)[0]["n"].c_str()));
    };
    const auto v1 = docver();   // bumped once by the INSERT
    const auto n1 = events();

    // Permission change: resource scope is unchanged, so OLD scope == NEW scope.
    exec_sync(db,
        "UPDATE resource_grants SET permission='write' "
        "WHERE company_id=$1::uuid AND resource_type='org_unit' AND resource_id=$2::uuid",
        std::string(CO), ouX);

    CHECK(docver() == v1 + 1);  // one bump, not two
    CHECK(events() == n1 + 1);  // one new resync event, not two
}
