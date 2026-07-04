#include "handlers.hpp"

#include "wikore/auth.hpp"           // Identity
#include "wikore/access.hpp"         // AccessService::effective_read_orgs
#include "wikore/adapters/postgres/error_mapper.hpp"  // map_db_exception

#include <drogon/orm/Exception.h>
#include <glaze/glaze.hpp>
#include <spdlog/spdlog.h>

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace wikore;
using drogon::HttpResponse;
using drogon::HttpResponsePtr;

namespace {

// Bounds (P2): the recursive builder is capped so a pathologically deep tenant
// hierarchy cannot exhaust the request thread's stack, and the query is capped
// so an enormous tenant cannot exhaust memory. Real org charts are shallow and
// small; these are defensive ceilings, not expected sizes.
constexpr int         kMaxDepth = 64;
constexpr std::size_t kMaxNodes = 10000;

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

// A forest: the caller may have several disjoint accessible subtrees (or, for a
// full-chart admin, a single tree rooted at the company root).
struct TreeResp {
    std::vector<OrgNode> orgs;
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

        // Tenant + company root from the authenticated (active) user - never
        // from the request. A deactivated user is rejected.
        std::string company_id, root_id;
        try {
            auto rows = co_await db->execSqlCoro(
                "SELECT u.company_id::text AS company_id, r.id::text AS root_id "
                "FROM users u "
                "JOIN org_units r ON r.company_id = u.company_id AND r.type = 'root' "
                "WHERE u.id = $1::uuid AND u.deactivated_at IS NULL", id.user_id);
            if (rows.empty())
                co_return json_error(drogon::k403Forbidden, "user not found or deactivated");
            company_id = rows[0]["company_id"].as<std::string>();
            root_id    = rows[0]["root_id"].as<std::string>();
        } catch (const drogon::orm::DrogonDbException& ex) {
            co_return db_error("tenant lookup", ex);
        }

        // Access model is default-closed: without membership a user sees
        // nothing. Non-admins are scoped to the org units their memberships
        // (and group memberships / grants) grant read access to, via the
        // canonical AccessService primitive. Admins get the full company chart.
        const bool full_chart = id.is_admin;
        std::unordered_set<std::string> accessible;
        if (!full_chart) {
            try {
                AccessService access(db);
                auto ids = co_await access.effective_read_orgs(
                    company_id, id.user_id, root_id);   // fail-closed (empty)
                accessible.insert(ids.begin(), ids.end());
            } catch (const drogon::orm::DrogonDbException& ex) {
                co_return db_error("scope resolution", ex);
            }
            if (accessible.empty())     // no membership -> empty forest
                co_return json(drogon::k200OK, R"({"orgs":[]})");
        }

        // All company org units (bounded), filtered to the visible set, then
        // assembled into a forest from parent_id.
        try {
            auto rows = co_await db->execSqlCoro(
                "SELECT id::text AS id, parent_id::text AS parent_id, type, slug, "
                "       name, description "
                "FROM org_units WHERE company_id = $1::uuid "
                "ORDER BY name, slug LIMIT $2", company_id,
                static_cast<long>(kMaxNodes + 1));

            if (rows.size() > kMaxNodes) {
                spdlog::error("[orgs_tree] company {} exceeds {} org units",
                              company_id, kMaxNodes);
                co_return json_error(drogon::k500InternalServerError, "org tree too large");
            }

            std::unordered_map<std::string, OrgNode>    nodes;
            std::unordered_map<std::string, std::string> parent_of;
            std::vector<std::string>                    order;   // name-sorted
            for (const auto& r : rows) {
                std::string oid = r["id"].as<std::string>();
                if (!full_chart && !accessible.contains(oid))
                    continue;   // default-closed: skip units the caller cannot see
                OrgNode n;
                n.id          = oid;
                n.type        = r["type"].as<std::string>();
                n.slug        = r["slug"].as<std::string>();
                n.name        = r["name"].as<std::string>();
                n.description = r["description"].isNull() ? "" : r["description"].as<std::string>();
                if (!r["parent_id"].isNull())
                    parent_of.emplace(oid, r["parent_id"].as<std::string>());
                nodes.emplace(oid, std::move(n));
                order.push_back(std::move(oid));
            }

            // A visible node is a forest root when it has no parent, or its
            // parent is not itself visible (so the caller sees a subtree
            // starting where their access begins).
            std::unordered_map<std::string, std::vector<std::string>> kids;
            std::vector<std::string> roots;
            for (const auto& oid : order) {
                auto pit = parent_of.find(oid);
                if (pit == parent_of.end() || !nodes.contains(pit->second))
                    roots.push_back(oid);
                else
                    kids[pit->second].push_back(oid);
            }

            std::function<OrgNode(const std::string&, int)> build =
                [&](const std::string& nid, int depth) -> OrgNode {
                    OrgNode n = nodes[nid];               // copy without children
                    if (depth < kMaxDepth)                // bound recursion/stack
                        if (auto it = kids.find(nid); it != kids.end())
                            for (const auto& child_id : it->second)
                                n.children.push_back(build(child_id, depth + 1));
                    return n;
                };

            TreeResp resp;
            resp.orgs.reserve(roots.size());
            for (const auto& r : roots)
                resp.orgs.push_back(build(r, 0));

            std::string out;
            if (glz::write_json(resp, out))
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
