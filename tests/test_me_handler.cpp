#include <catch2/catch_test_macros.hpp>
#include "handlers.hpp"          // wikore::api::me
#include "wikore/auth.hpp"      // Identity
#include "wikore/db.hpp"

#include <drogon/drogon.h>
#include <drogon/utils/coroutine.h>
#include <glaze/glaze.hpp>

#include <cstdlib>
#include <string>

// Integration test for GET /api/me. Drives wikore::api::me directly with a real
// Postgres (the executable is not linkable from tests). Skips without
// DATABASE_URL.

namespace {

constexpr auto CO = "05c00000-0000-0000-0000-0000000000c1";

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

std::string seed_user(drogon::orm::DbClientPtr db)
{
    exec_sync(db, "DELETE FROM companies WHERE id=$1::uuid", std::string(CO));
    exec_sync(db, "INSERT INTO companies (id,name,slug) VALUES ($1::uuid,'MeCo','me-co')",
              std::string(CO));
    return std::string(exec_sync(db,
        "INSERT INTO users (company_id,external_sub,email,display_name) "
        "VALUES ($1::uuid,'subMe','me@test','Me User') RETURNING id",
        std::string(CO))[0]["id"].c_str());
}

drogon::HttpRequestPtr req_with_identity(const std::string& user_id)
{
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Get);
    wikore::Identity id;
    id.user_id      = user_id;
    id.email        = "me@test";
    id.display_name = "Me User";
    id.is_admin     = true;
    req->getAttributes()->insert("identity", id);
    return req;
}

struct MeResp {
    std::string user_id;
    std::string email;
    std::string display_name;
    bool        is_admin = false;
    std::string company_id;
};

// Sets a client-wide query timeout for the scope and restores the configured
// value on destruction (even on exception), so a test that shortens the shared
// client's timeout cannot leak an unbounded/changed timeout to later tests.
// The restore value mirrors Db::init's PostgresConfig.timeout (30s).
struct ClientTimeoutGuard {
    drogon::orm::DbClientPtr db;
    ClientTimeoutGuard(drogon::orm::DbClientPtr d, double set) : db(std::move(d)) {
        db->setTimeout(set);
    }
    ~ClientTimeoutGuard() { db->setTimeout(30.0); }
};

} // namespace

TEST_CASE("me: returns the caller's identity and tenant (200)", "[integration][api]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db   = wikore::Db::get();
    auto user = seed_user(db);

    auto resp = drogon::sync_wait(wikore::api::me(db, req_with_identity(user)));
    REQUIRE(resp->getStatusCode() == drogon::k200OK);

    MeResp r;
    REQUIRE_FALSE(glz::read<glz::opts{.error_on_unknown_keys = false}>(
        r, std::string(resp->getBody())));
    CHECK(r.user_id    == user);
    CHECK(r.company_id == CO);
    CHECK(r.email      == "me@test");
    CHECK(r.is_admin   == true);
}

TEST_CASE("me: missing identity attribute is 401", "[integration][api]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    (void)seed_user(db);

    auto req = drogon::HttpRequest::newHttpRequest();  // no "identity" attribute
    auto resp = drogon::sync_wait(wikore::api::me(db, req));
    CHECK(resp->getStatusCode() == drogon::k401Unauthorized);
}

TEST_CASE("me: a deactivated user is rejected (403)", "[integration][api]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db   = wikore::Db::get();
    auto user = seed_user(db);
    exec_sync(db, "UPDATE users SET deactivated_at = now() WHERE id = $1::uuid", user);

    auto resp = drogon::sync_wait(wikore::api::me(db, req_with_identity(user)));
    CHECK(resp->getStatusCode() == drogon::k403Forbidden);
}

TEST_CASE("me: a database timeout maps to 503, not 500", "[integration][api]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db   = wikore::Db::get();
    auto user = seed_user(db);

    // Shorten the shared client's timeout for this scope only (RAII-restored),
    // then hold an ACCESS EXCLUSIVE lock on users in a background transaction so
    // me()'s SELECT blocks. The timeout fires a TimeoutError, which must map to
    // 503 (not 500) - as on the read path.
    ClientTimeoutGuard guard(db, /*set=*/0.5);

    const auto status = drogon::sync_wait(
        [&]() -> drogon::Task<drogon::HttpStatusCode> {
            auto locker = co_await db->newTransactionCoro();
            co_await locker->execSqlCoro("LOCK TABLE users IN ACCESS EXCLUSIVE MODE");
            auto resp = co_await wikore::api::me(db, req_with_identity(user));
            // locker leaves scope here -> rollback releases the lock.
            co_return resp->getStatusCode();
        }());

    CHECK(status == drogon::k503ServiceUnavailable);
}
