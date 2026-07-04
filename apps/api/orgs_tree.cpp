#include "handlers.hpp"

#include "wikore/auth.hpp"             // Identity
#include "wikore/access_resolver.hpp"  // PostgresAccessResolver (error-surfacing)
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

// Bounds (P2). Real org charts are shallow and small; these are defensive
// ceilings. The node cap is enforced on the company's TOTAL org-unit count
// BEFORE scope resolution, so a root-scoped membership cannot make scope
// resolution expand an unbounded subtree first. The depth cap protects the
// request thread's stack, and exceeding either is an explicit error, never a
// silently truncated tree presented as complete.
constexpr int  kMaxDepth = 64;
constexpr long kMaxNodes = 10000;

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

drogon::HttpStatusCode status_for(Error::Kind k)
{
    switch (k) {
        case Error::Kind::ServiceUnavailable: return drogon::k503ServiceUnavailable;
        case Error::Kind::NotFound:           return drogon::k404NotFound;
        case Error::Kind::Forbidden:          return drogon::k403Forbidden;
        case Error::Kind::InvalidInput:       return drogon::k400BadRequest;
        default:                              return drogon::k500InternalServerError;
    }
}

std::string_view client_msg(drogon::HttpStatusCode code)
{
    return code == drogon::k503ServiceUnavailable ? "service temporarily unavailable"
         : code >= drogon::k500InternalServerError ? "internal error"
         : "request could not be completed";
}

// A DB timeout (client query timeout) maps to 503; everything else to 500 -
// consistent with the read path and /api/me.
HttpResponsePtr db_error(std::string_view where, const drogon::orm::DrogonDbException& ex)
{
    const auto code = status_for(postgres::map_db_exception(ex).kind);
    spdlog::error("[orgs_tree] {} failed ({}): {}", where,
                  static_cast<int>(code), ex.base().what());
    return json_error(code, client_msg(code));
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

        // The node-count gate, scope resolution, and the fetch all run in ONE
        // REPEATABLE READ transaction, so they share a single snapshot. That
        // closes the TOCTOU gap where rows inserted between a separate count and
        // fetch could bypass the cap, and bounds scope resolution against the
        // very snapshot the count measured. drogon's Transaction is a DbClient,
        // so PostgresAccessResolver runs on it (reused, not re-implemented).
        const bool full_chart = id.is_admin;
        std::unordered_set<std::string> accessible;
        try {
            auto tx = co_await db->newTransactionCoro();
            co_await tx->execSqlCoro("SET TRANSACTION ISOLATION LEVEL REPEATABLE READ");

            // Tenant + company root + total org-unit count. A deactivated user
            // is rejected; the count is the memory bound for everything below,
            // and (in this snapshot) exactly what the fetch will return.
            std::string company_id, root_id;
            {
                auto trows = co_await tx->execSqlCoro(
                    "SELECT u.company_id::text AS company_id, r.id::text AS root_id, "
                    "       (SELECT count(*) FROM org_units o WHERE o.company_id = u.company_id) "
                    "         AS n "
                    "FROM users u "
                    "JOIN org_units r ON r.company_id = u.company_id AND r.type = 'root' "
                    "WHERE u.id = $1::uuid AND u.deactivated_at IS NULL", id.user_id);
                if (trows.empty())
                    co_return json_error(drogon::k403Forbidden, "user not found or deactivated");
                company_id = trows[0]["company_id"].as<std::string>();
                root_id    = trows[0]["root_id"].as<std::string>();
                if (trows[0]["n"].as<long>() > kMaxNodes) {
                    spdlog::error("[orgs_tree] company {} exceeds {} org units",
                                  company_id, kMaxNodes);
                    co_return json_error(drogon::k500InternalServerError, "org tree too large");
                }
            }

            // Default-closed scope for non-admins, resolved on the SAME snapshot
            // (so it is bounded by the count above). PostgresAccessResolver
            // (not AccessService::effective_read_orgs, which swallows DB errors
            // as an empty scope) SURFACES a timeout/outage, so it becomes 503
            // rather than a false 200 empty tree. Admins get the full chart.
            if (!full_chart) {
                auto scope = co_await PostgresAccessResolver(tx).resolve(
                    company_id, id.user_id, root_id);
                if (!scope) {
                    const auto code = status_for(scope.error().kind);
                    spdlog::warn("[orgs_tree] scope resolution failed ({}): {}",
                                 static_cast<int>(code), scope.error().message);
                    co_return json_error(code, client_msg(code));
                }
                accessible.insert(scope->org_unit_ids.begin(), scope->org_unit_ids.end());
                if (accessible.empty())     // no membership -> empty forest
                    co_return json(drogon::k200OK, R"({"orgs":[]})");
            }

            // Company org units (same snapshot -> exactly the counted rows),
            // filtered to the visible set, then assembled into a forest.
            auto rows = co_await tx->execSqlCoro(
                "SELECT id::text AS id, parent_id::text AS parent_id, type, slug, "
                "       name, description "
                "FROM org_units WHERE company_id = $1::uuid "
                "ORDER BY name, slug", company_id);

            std::unordered_map<std::string, OrgNode>     nodes;
            std::unordered_map<std::string, std::string> parent_of;
            std::vector<std::string>                     order;   // name-sorted
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

            // Bounded recursive build. If the hierarchy is deeper than kMaxDepth
            // we STOP descending (stack safety) and flag it, so we return an
            // explicit error rather than a silently truncated tree.
            bool too_deep = false;
            std::function<OrgNode(const std::string&, int)> build =
                [&](const std::string& nid, int depth) -> OrgNode {
                    OrgNode n = nodes[nid];               // copy without children
                    if (auto it = kids.find(nid); it != kids.end()) {
                        if (depth + 1 > kMaxDepth)
                            too_deep = true;              // would exceed the cap
                        else
                            for (const auto& child_id : it->second)
                                n.children.push_back(build(child_id, depth + 1));
                    }
                    return n;
                };

            TreeResp resp;
            resp.orgs.reserve(roots.size());
            for (const auto& r : roots)
                resp.orgs.push_back(build(r, 0));

            if (too_deep) {
                spdlog::error("[orgs_tree] company {} tree exceeds depth {}",
                              company_id, kMaxDepth);
                co_return json_error(drogon::k500InternalServerError, "org tree too deep");
            }

            std::string out;
            if (glz::write_json(resp, out))
                co_return json_error(drogon::k500InternalServerError, "internal error");
            co_return json(drogon::k200OK, std::move(out));
        } catch (const drogon::orm::DrogonDbException& ex) {
            co_return db_error("org tree query", ex);
        }

    } catch (const std::exception& ex) {
        spdlog::error("[orgs_tree] unhandled exception: {}", ex.what());
        co_return json_error(drogon::k500InternalServerError, "internal error");
    } catch (...) {
        spdlog::error("[orgs_tree] unhandled non-standard exception");
        co_return json_error(drogon::k500InternalServerError, "internal error");
    }
}
