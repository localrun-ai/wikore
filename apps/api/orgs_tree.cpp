#include "handlers.hpp"

#include "wikore/auth.hpp"           // Identity
#include "wikore/adapters/postgres/error_mapper.hpp"  // map_db_exception

#include <drogon/orm/Exception.h>
#include <glaze/glaze.hpp>
#include <spdlog/spdlog.h>

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

using namespace wikore;
using drogon::HttpResponse;
using drogon::HttpResponsePtr;

namespace {

// Recursive node; glaze reflects the member names into JSON keys and handles
// the self-referential children vector.
struct OrgNode {
    std::string          id;
    std::string          type;         // 'root' | 'subsidiary' | ... (raw)
    std::string          slug;
    std::string          name;
    std::string          description;
    std::vector<OrgNode> children;
};

struct TreeResp {
    OrgNode tree;
};

struct ErrDto {
    std::string error;
};

HttpResponsePtr json(drogon::HttpStatusCode code, std::string body)
{
    auto r = HttpResponse::newHttpResponse();
    r->setStatusCode(code);
    r->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    r->setBody(std::move(body));
    return r;
}

HttpResponsePtr json_error(drogon::HttpStatusCode code, std::string_view msg)
{
    std::string out;
    if (glz::write_json(ErrDto{std::string(msg)}, out))
        out = R"({"error":"internal error"})";
    return json(code, std::move(out));
}

// A DB timeout (client query timeout) maps to 503; everything else to 500 -
// consistent with the read path and /api/me.
HttpResponsePtr db_error(std::string_view where, const drogon::orm::DrogonDbException& ex)
{
    const auto e    = postgres::map_db_exception(ex);
    const auto code = e.kind == Error::Kind::ServiceUnavailable
                        ? drogon::k503ServiceUnavailable
                        : drogon::k500InternalServerError;
    spdlog::error("[orgs_tree] {} failed ({}): {}", where,
                  static_cast<int>(code), ex.base().what());
    return json_error(code,
        code == drogon::k503ServiceUnavailable ? "service temporarily unavailable"
                                               : "internal error");
}

} // namespace

drogon::Task<HttpResponsePtr>
wikore::api::orgs_tree(drogon::orm::DbClientPtr db, drogon::HttpRequestPtr req)
{
    try {
        // AuthFilter guarantees identity; guard defensively.
        if (!req->getAttributes()->find("identity"))
            co_return json_error(drogon::k401Unauthorized, "unauthenticated");
        const auto id = req->getAttributes()->get<Identity>("identity");

        // Tenant from the authenticated (active) user - never from the request.
        std::string company_id;
        try {
            auto rows = co_await db->execSqlCoro(
                "SELECT company_id FROM users "
                "WHERE id=$1::uuid AND deactivated_at IS NULL", id.user_id);
            if (rows.empty())
                co_return json_error(drogon::k403Forbidden, "user not found or deactivated");
            company_id = rows[0]["company_id"].as<std::string>();
        } catch (const drogon::orm::DrogonDbException& ex) {
            co_return db_error("tenant lookup", ex);
        }

        // All org units for the company in one indexed read, assembled into a
        // nested tree in memory from parent_id. Returning the whole company
        // structure (org chart) is company-visible metadata, so it is not
        // per-caller access-scoped (only tenant-scoped).
        try {
            auto rows = co_await db->execSqlCoro(
                "SELECT id::text AS id, parent_id::text AS parent_id, type, slug, "
                "       name, description "
                "FROM org_units WHERE company_id=$1::uuid "
                "ORDER BY name, slug", company_id);

            // Flat node data + a parent -> child-ids index; then materialize the
            // nested tree from the root (parent_id IS NULL).
            std::unordered_map<std::string, OrgNode>                  nodes;
            std::unordered_map<std::string, std::vector<std::string>> kids;
            std::string root_id;
            for (const auto& r : rows) {
                OrgNode n;
                n.id          = r["id"].as<std::string>();
                n.type        = r["type"].as<std::string>();
                n.slug        = r["slug"].as<std::string>();
                n.name        = r["name"].as<std::string>();
                n.description = r["description"].isNull() ? "" : r["description"].as<std::string>();
                if (r["parent_id"].isNull())
                    root_id = n.id;
                else
                    kids[r["parent_id"].as<std::string>()].push_back(n.id);
                nodes.emplace(n.id, std::move(n));
            }

            if (root_id.empty())   // every company has a root (DB trigger); defensive
                co_return json_error(drogon::k500InternalServerError, "internal error");

            std::function<OrgNode(const std::string&)> build =
                [&](const std::string& node_id) -> OrgNode {
                    OrgNode n = nodes[node_id];          // copy without children
                    if (auto it = kids.find(node_id); it != kids.end())
                        for (const auto& child_id : it->second)
                            n.children.push_back(build(child_id));
                    return n;
                };

            std::string out;
            if (glz::write_json(TreeResp{build(root_id)}, out))
                co_return json_error(drogon::k500InternalServerError, "internal error");
            co_return json(drogon::k200OK, std::move(out));
        } catch (const drogon::orm::DrogonDbException& ex) {
            co_return db_error("org_units query", ex);
        }

    } catch (const std::exception& ex) {
        spdlog::error("[orgs_tree] unhandled exception: {}", ex.what());
        co_return json_error(drogon::k500InternalServerError, "internal error");
    } catch (...) {
        spdlog::error("[orgs_tree] unhandled non-standard exception");
        co_return json_error(drogon::k500InternalServerError, "internal error");
    }
}
