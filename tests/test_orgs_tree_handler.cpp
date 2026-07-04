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

struct Fixture {
    std::string root, eng, hr, backend, user;
};

// company root -> {Engineering -> {Backend}, HR}
Fixture seed(drogon::orm::DbClientPtr db)
{
    exec_sync(db, "DELETE FROM companies WHERE id=$1::uuid", std::string(CO));
    exec_sync(db, "DELETE FROM companies WHERE id=$1::uuid", std::string(CO2));
    exec_sync(db, "INSERT INTO companies (id,name,slug) VALUES ($1::uuid,'TreeCo','tree-co')",
              std::string(CO));
    // A second tenant with its own units - must never appear in CO's tree.
    exec_sync(db, "INSERT INTO companies (id,name,slug) VALUES ($1::uuid,'OtherCo','other-co')",
              std::string(CO2));

    Fixture f;
    f.root    = root_of(db, CO);
    f.eng     = make_ou(db, CO, f.root, "Engineering");
    f.hr      = make_ou(db, CO, f.root, "HR");
    f.backend = make_ou(db, CO, f.eng,  "Backend");
    make_ou(db, CO2, root_of(db, CO2), "ForeignDept");   // other tenant noise

    f.user = std::string(exec_sync(db,
        "INSERT INTO users (company_id,external_sub,email) "
        "VALUES ($1::uuid,'subTree','tree@test') RETURNING id",
        std::string(CO))[0]["id"].c_str());
    return f;
}

drogon::HttpRequestPtr req_with_identity(const std::string& user_id)
{
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Get);
    wikore::Identity id;
    id.user_id = user_id;
    id.email   = "tree@test";
    req->getAttributes()->insert("identity", id);
    return req;
}

struct Node {
    std::string       id;
    std::string       type;
    std::string       slug;
    std::string       name;
    std::string       description;
    std::vector<Node> children;
};
struct Resp { Node tree; };

const Node* find(const Node& n, const std::string& slug)
{
    if (n.slug == slug) return &n;
    for (const auto& c : n.children)
        if (const Node* hit = find(c, slug)) return hit;
    return nullptr;
}

} // namespace

TEST_CASE("orgs_tree: returns the company hierarchy, tenant-scoped (200)",
          "[integration][api]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    auto f  = seed(db);

    auto resp = drogon::sync_wait(wikore::api::orgs_tree(db, req_with_identity(f.user)));
    REQUIRE(resp->getStatusCode() == drogon::k200OK);

    Resp r;
    REQUIRE_FALSE(glz::read<glz::opts{.error_on_unknown_keys = false}>(
        r, std::string(resp->getBody())));

    CHECK(r.tree.id   == f.root);
    CHECK(r.tree.type == "root");
    // Engineering and HR are direct children of root.
    CHECK(find(r.tree, "Engineering") != nullptr);
    CHECK(find(r.tree, "HR")          != nullptr);
    // Backend nests under Engineering, not root.
    const Node* eng = find(r.tree, "Engineering");
    REQUIRE(eng != nullptr);
    bool backend_under_eng = false;
    for (const auto& c : eng->children) if (c.slug == "Backend") backend_under_eng = true;
    CHECK(backend_under_eng);
    // No cross-tenant leak.
    CHECK(find(r.tree, "ForeignDept") == nullptr);
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
    exec_sync(db, "UPDATE users SET deactivated_at = now() WHERE id = $1::uuid", f.user);

    auto resp = drogon::sync_wait(wikore::api::orgs_tree(db, req_with_identity(f.user)));
    CHECK(resp->getStatusCode() == drogon::k403Forbidden);
}
