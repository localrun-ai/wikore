#include <catch2/catch_test_macros.hpp>
#include "handlers.hpp"          // wikore::api::org_get
#include "wikore/auth.hpp"      // Identity
#include "wikore/db.hpp"

#include <drogon/drogon.h>
#include <drogon/utils/coroutine.h>
#include <glaze/glaze.hpp>

#include <cstdlib>
#include <optional>
#include <string>

// Integration test for GET /api/orgs/{orgUnitId}. Drives wikore::api::org_get
// directly against a real Postgres. Skips without DATABASE_URL. Default-closed:
// a non-admin reads a unit only within their membership scope.

namespace {

constexpr auto CO  = "05c00000-0000-0000-0000-0000000000e1";  // this tenant
constexpr auto CO2 = "05c00000-0000-0000-0000-0000000000e2";  // other tenant

bool db_available() { return std::getenv("DATABASE_URL") != nullptr; }

template<typename... Args>
drogon::orm::Result exec_sync(drogon::orm::DbClientPtr db, std::string sql, Args... args)
{
    return drogon::sync_wait(
        [db, sql = std::move(sql), ...args = std::move(args)]()
        -> drogon::Task<drogon::orm::Result> {
            co_return co_await db->execSqlCoro(sql, args...);
        }());
}

std::string root_of(drogon::orm::DbClientPtr db, const char* co)
{
    return std::string(exec_sync(db,
        "SELECT id FROM org_units WHERE company_id=$1::uuid AND type='root'",
        std::string(co))[0]["id"].c_str());
}

std::string make_ou(drogon::orm::DbClientPtr db, const char* co,
                    const std::string& parent, const std::string& slug)
{
    return std::string(exec_sync(db,
        "INSERT INTO org_units (company_id,parent_id,type,slug,name) "
        "VALUES ($1::uuid,$2::uuid,'department',$3,$3) RETURNING id",
        std::string(co), parent, slug)[0]["id"].c_str());
}

std::string make_user(drogon::orm::DbClientPtr db, const char* co, const char* sub)
{
    return std::string(exec_sync(db,
        "INSERT INTO users (company_id,external_sub,email) "
        "VALUES ($1::uuid,$2,$2||'@test') RETURNING id",
        std::string(co), std::string(sub))[0]["id"].c_str());
}

void grant(drogon::orm::DbClientPtr db, const char* co, const std::string& user,
           const std::string& ou, const char* applies_to)
{
    exec_sync(db,
        "INSERT INTO memberships (company_id,user_id,org_unit_id,role,applies_to) "
        "VALUES ($1::uuid,$2::uuid,$3::uuid,'viewer',$4)",
        std::string(co), user, ou, std::string(applies_to));
}

struct Fixture { std::string root, eng, hr, backend, foreign; };

Fixture seed(drogon::orm::DbClientPtr db)
{
    exec_sync(db, "DELETE FROM companies WHERE id=$1::uuid", std::string(CO));
    exec_sync(db, "DELETE FROM companies WHERE id=$1::uuid", std::string(CO2));
    exec_sync(db, "INSERT INTO companies (id,name,slug) VALUES ($1::uuid,'GetCo','get-co')",
              std::string(CO));
    exec_sync(db, "INSERT INTO companies (id,name,slug) VALUES ($1::uuid,'OtherCo','other-co2')",
              std::string(CO2));
    Fixture f;
    f.root    = root_of(db, CO);
    f.eng     = make_ou(db, CO, f.root, "Engineering");
    f.hr      = make_ou(db, CO, f.root, "HR");
    f.backend = make_ou(db, CO, f.eng,  "Backend");
    f.foreign = make_ou(db, CO2, root_of(db, CO2), "ForeignDept");
    return f;
}

drogon::HttpRequestPtr req_as(const std::string& user_id, bool admin = false)
{
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Get);
    wikore::Identity id;
    id.user_id  = user_id;
    id.email    = "u@test";
    id.is_admin = admin;
    req->getAttributes()->insert("identity", id);
    return req;
}

int status(const std::string& user, const std::string& ou, bool admin = false)
{
    auto db = wikore::Db::get();
    return static_cast<int>(
        drogon::sync_wait(wikore::api::org_get(db, req_as(user, admin), ou))
            ->getStatusCode());
}

struct OrgDto {
    std::string                id;
    std::optional<std::string> parent_id;
    std::string                type, slug, name, description;
};

struct ClientTimeoutGuard {
    drogon::orm::DbClientPtr db;
    ClientTimeoutGuard(drogon::orm::DbClientPtr d, double set) : db(std::move(d)) {
        db->setTimeout(set);
    }
    ~ClientTimeoutGuard() { db->setTimeout(30.0); }
};

} // namespace

TEST_CASE("org_get: a member reads units within their scope, 404 outside it",
          "[integration][api]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    auto f  = seed(db);
    auto user = make_user(db, CO, "e-mgr");
    grant(db, CO, user, f.eng, "self_and_descendants");

    // In scope: Engineering (self) + Backend (descendant).
    auto resp = drogon::sync_wait(wikore::api::org_get(db, req_as(user), f.eng));
    REQUIRE(resp->getStatusCode() == drogon::k200OK);
    OrgDto dto;
    REQUIRE_FALSE(glz::read<glz::opts{.error_on_unknown_keys = false}>(
        dto, std::string(resp->getBody())));
    CHECK(dto.id == f.eng);
    CHECK(dto.slug == "Engineering");
    CHECK(dto.parent_id == f.root);

    CHECK(status(user, f.backend) == 200);
    // Out of scope: HR and root -> 404 (no existence leak, not 403).
    CHECK(status(user, f.hr)   == 404);
    CHECK(status(user, f.root) == 404);
}

TEST_CASE("org_get: self_only membership does not expose a descendant",
          "[integration][api]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    auto f  = seed(db);
    auto user = make_user(db, CO, "e-self");
    grant(db, CO, user, f.eng, "self_only");

    CHECK(status(user, f.eng)     == 200);
    CHECK(status(user, f.backend) == 404);
}

TEST_CASE("org_get: no membership is 404 (default-closed)", "[integration][api]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    auto f  = seed(db);
    auto user = make_user(db, CO, "nobody");
    CHECK(status(user, f.eng) == 404);
}

TEST_CASE("org_get: an admin reads any unit in their tenant, not another tenant's",
          "[integration][api]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    auto f  = seed(db);
    auto user = make_user(db, CO, "admin");
    CHECK(status(user, f.root, /*admin=*/true) == 200);
    CHECK(status(user, f.hr,   /*admin=*/true) == 200);
    CHECK(status(user, f.foreign, /*admin=*/true) == 404);   // cross-tenant
}

TEST_CASE("org_get: a foreign-tenant unit is 404 for a scoped member",
          "[integration][api]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    auto f  = seed(db);
    auto user = make_user(db, CO, "e-mgr2");
    grant(db, CO, user, f.eng, "self_and_descendants");
    CHECK(status(user, f.foreign) == 404);
}

TEST_CASE("org_get: malformed id, missing identity, deactivated user",
          "[integration][api]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    auto f  = seed(db);
    auto user = make_user(db, CO, "e-mgr3");
    grant(db, CO, user, f.eng, "self_and_descendants");

    SECTION("malformed uuid -> 404") {
        CHECK(status(user, "not-a-uuid") == 404);
    }
    SECTION("missing identity -> 401") {
        auto req = drogon::HttpRequest::newHttpRequest();
        auto resp = drogon::sync_wait(wikore::api::org_get(db, req, f.eng));
        CHECK(resp->getStatusCode() == drogon::k401Unauthorized);
    }
    SECTION("deactivated user -> 403") {
        exec_sync(db, "UPDATE users SET deactivated_at=now() WHERE id=$1::uuid", user);
        CHECK(status(user, f.eng) == 403);
    }
}

TEST_CASE("org_get: a scope-resolution DB timeout is 503, not a false 404",
          "[integration][api]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    auto f  = seed(db);
    auto user = make_user(db, CO, "e-timeout");
    grant(db, CO, user, f.eng, "self_and_descendants");

    // Lock `memberships` so scope resolution blocks (the org-unit fetch touches
    // only org_units and still succeeds); a short client timeout then fires and
    // the resolver surfaces it -> 503, not a false 404 from a swallowed error.
    ClientTimeoutGuard guard(db, 0.5);
    const auto st = drogon::sync_wait(
        [&]() -> drogon::Task<drogon::HttpStatusCode> {
            auto locker = co_await db->newTransactionCoro();
            co_await locker->execSqlCoro("LOCK TABLE memberships IN ACCESS EXCLUSIVE MODE");
            auto resp = co_await wikore::api::org_get(db, req_as(user), f.eng);
            co_return resp->getStatusCode();
        }());

    CHECK(st == drogon::k503ServiceUnavailable);
}
