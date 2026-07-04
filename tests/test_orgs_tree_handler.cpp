#include <catch2/catch_test_macros.hpp>
#include "handlers.hpp"          // wikore::api::orgs_tree
#include "wikore/auth.hpp"      // Identity
#include "wikore/db.hpp"

#include <drogon/drogon.h>
#include <drogon/utils/coroutine.h>
#include <glaze/glaze.hpp>

#include <cstdlib>
#include <string>
#include <vector>

// Integration test for GET /api/orgs/tree. Drives wikore::api::orgs_tree
// directly against a real Postgres (the executable is not linkable from tests).
// Skips without DATABASE_URL.
//
// Access model is default-closed: a user sees only the org units their
// memberships grant (a forest of accessible subtrees); admins see the whole
// company chart.

namespace {

constexpr auto CO  = "05c00000-0000-0000-0000-0000000000d1";  // this tenant
constexpr auto CO2 = "05c00000-0000-0000-0000-0000000000d2";  // other tenant

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

struct Fixture {
    std::string root, eng, hr, backend;
};

// company root -> {Engineering -> {Backend}, HR}; plus a foreign tenant.
Fixture seed(drogon::orm::DbClientPtr db)
{
    exec_sync(db, "DELETE FROM companies WHERE id=$1::uuid", std::string(CO));
    exec_sync(db, "DELETE FROM companies WHERE id=$1::uuid", std::string(CO2));
    exec_sync(db, "INSERT INTO companies (id,name,slug) VALUES ($1::uuid,'TreeCo','tree-co')",
              std::string(CO));
    exec_sync(db, "INSERT INTO companies (id,name,slug) VALUES ($1::uuid,'OtherCo','other-co')",
              std::string(CO2));

    Fixture f;
    f.root    = root_of(db, CO);
    f.eng     = make_ou(db, CO, f.root, "Engineering");
    f.hr      = make_ou(db, CO, f.root, "HR");
    f.backend = make_ou(db, CO, f.eng,  "Backend");
    make_ou(db, CO2, root_of(db, CO2), "ForeignDept");   // other-tenant noise
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

struct Node {
    std::string       id, type, slug, name, description;
    std::vector<Node> children;
};
struct Resp { std::vector<Node> orgs; };

Resp parse(const drogon::HttpResponsePtr& resp)
{
    Resp r;
    (void)glz::read<glz::opts{.error_on_unknown_keys = false}>(r, std::string(resp->getBody()));
    return r;
}

const Node* find(const std::vector<Node>& forest, const std::string& slug)
{
    for (const auto& n : forest) {
        if (n.slug == slug) return &n;
        if (const Node* hit = find(n.children, slug)) return hit;
    }
    return nullptr;
}

} // namespace

TEST_CASE("orgs_tree: a member sees only their scoped subtree", "[integration][api]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    auto f  = seed(db);

    // Member of Engineering with descendants -> sees Engineering + Backend,
    // NOT HR, and NOT the company root (no membership there).
    auto user = make_user(db, CO, "eng-mgr");
    grant(db, CO, user, f.eng, "self_and_descendants");

    auto resp = drogon::sync_wait(wikore::api::orgs_tree(db, req_as(user)));
    REQUIRE(resp->getStatusCode() == drogon::k200OK);
    auto r = parse(resp);

    REQUIRE(r.orgs.size() == 1);          // one accessible subtree
    CHECK(r.orgs[0].slug == "Engineering");
    CHECK(find(r.orgs, "Backend") != nullptr);   // nested under Engineering
    CHECK(find(r.orgs, "HR")   == nullptr);      // not accessible
    CHECK(find(r.orgs, "TreeCo") == nullptr);    // root not accessible
    CHECK(find(r.orgs, "ForeignDept") == nullptr);
}

TEST_CASE("orgs_tree: self_only membership does not expose descendants",
          "[integration][api]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    auto f  = seed(db);

    auto user = make_user(db, CO, "eng-self");
    grant(db, CO, user, f.eng, "self_only");

    auto r = parse(drogon::sync_wait(wikore::api::orgs_tree(db, req_as(user))));
    REQUIRE(r.orgs.size() == 1);
    CHECK(r.orgs[0].slug == "Engineering");
    CHECK(r.orgs[0].children.empty());           // Backend NOT visible
    CHECK(find(r.orgs, "Backend") == nullptr);
}

TEST_CASE("orgs_tree: no membership yields an empty forest (default-closed)",
          "[integration][api]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    (void)seed(db);
    auto user = make_user(db, CO, "no-member");

    auto resp = drogon::sync_wait(wikore::api::orgs_tree(db, req_as(user)));
    CHECK(resp->getStatusCode() == drogon::k200OK);
    CHECK(parse(resp).orgs.empty());
}

TEST_CASE("orgs_tree: an admin sees the whole company chart", "[integration][api]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    auto f  = seed(db);
    auto user = make_user(db, CO, "admin-user");   // no membership, but is_admin

    auto r = parse(drogon::sync_wait(wikore::api::orgs_tree(db, req_as(user, /*admin=*/true))));
    REQUIRE(r.orgs.size() == 1);
    CHECK(r.orgs[0].id == f.root);
    CHECK(r.orgs[0].type == "root");
    CHECK(find(r.orgs, "Engineering") != nullptr);
    CHECK(find(r.orgs, "HR")          != nullptr);
    CHECK(find(r.orgs, "Backend")     != nullptr);
    CHECK(find(r.orgs, "ForeignDept") == nullptr);   // still tenant-scoped
}

TEST_CASE("orgs_tree: missing identity is 401", "[integration][api]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    (void)seed(db);
    auto req  = drogon::HttpRequest::newHttpRequest();  // no identity attribute
    auto resp = drogon::sync_wait(wikore::api::orgs_tree(db, req));
    CHECK(resp->getStatusCode() == drogon::k401Unauthorized);
}

TEST_CASE("orgs_tree: a deactivated user is rejected (403)", "[integration][api]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    auto f  = seed(db);
    auto user = make_user(db, CO, "gone");
    grant(db, CO, user, f.eng, "self_and_descendants");
    exec_sync(db, "UPDATE users SET deactivated_at = now() WHERE id = $1::uuid", user);

    auto resp = drogon::sync_wait(wikore::api::orgs_tree(db, req_as(user)));
    CHECK(resp->getStatusCode() == drogon::k403Forbidden);
}
