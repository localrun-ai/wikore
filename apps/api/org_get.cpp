#include "handlers.hpp"

#include "wikore/auth.hpp"             // Identity
#include "wikore/access_resolver.hpp"  // PostgresAccessResolver (error-surfacing)
#include "wikore/adapters/postgres/error_mapper.hpp"  // map_db_exception

#include <drogon/orm/Exception.h>
#include <glaze/glaze.hpp>
#include <spdlog/spdlog.h>

#include <cctype>
#include <optional>
#include <string>
#include <string_view>

using namespace wikore;
using drogon::HttpResponse;
using drogon::HttpResponsePtr;

namespace {

struct OrgDto {
    std::string                id;
    std::optional<std::string> parent_id;    // null for the company root
    std::string                type;
    std::string                slug;
    std::string                name;
    std::string                description;
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

HttpResponsePtr db_error(std::string_view where, const drogon::orm::DrogonDbException& ex)
{
    const auto code = status_for(postgres::map_db_exception(ex).kind);
    spdlog::error("[org_get] {} failed ({}): {}", where,
                  static_cast<int>(code), ex.base().what());
    return json_error(code, client_msg(code));
}

// Canonical 8-4-4-4-12 UUID shape; guards the untrusted path parameter before a
// ::uuid cast (which would otherwise surface as 500 instead of 404).
bool looks_like_uuid(std::string_view s)
{
    if (s.size() != 36) return false;
    for (std::size_t i = 0; i < 36; ++i) {
        const char c = s[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') return false;
        } else if (!std::isxdigit(static_cast<unsigned char>(c))) {
            return false;
        }
    }
    return true;
}

} // namespace

drogon::Task<HttpResponsePtr>
wikore::api::org_get(drogon::orm::DbClientPtr db, drogon::HttpRequestPtr req,
                     std::string org_unit_id)
{
    try {
        // AuthFilter guarantees identity; guard defensively.
        if (!req->getAttributes()->find("identity"))
            co_return json_error(drogon::k401Unauthorized, "unauthenticated");
        const auto id = req->getAttributes()->get<Identity>("identity");

        // Reject a malformed id before it reaches a ::uuid cast.
        if (!looks_like_uuid(org_unit_id))
            co_return json_error(drogon::k404NotFound, "org unit not found");

        // Tenant, existence, and access checked on ONE snapshot, so an access
        // decision cannot disagree with the row that was fetched.
        try {
            auto tx = co_await db->newTransactionCoro();
            co_await tx->execSqlCoro("SET TRANSACTION ISOLATION LEVEL REPEATABLE READ");

            // Tenant from the authenticated (active) user - never the request.
            std::string company_id;
            {
                auto trows = co_await tx->execSqlCoro(
                    "SELECT company_id::text AS company_id FROM users "
                    "WHERE id = $1::uuid AND deactivated_at IS NULL", id.user_id);
                if (trows.empty())
                    co_return json_error(drogon::k403Forbidden, "user not found or deactivated");
                company_id = trows[0]["company_id"].as<std::string>();
            }

            // Fetch the org unit, tenant-scoped. A miss is 404 (absent or a
            // different tenant - existence never leaks across tenants).
            auto orows = co_await tx->execSqlCoro(
                "SELECT id::text AS id, parent_id::text AS parent_id, type, slug, "
                "       name, description "
                "FROM org_units WHERE id = $1::uuid AND company_id = $2::uuid",
                org_unit_id, company_id);
            if (orows.empty())
                co_return json_error(drogon::k404NotFound, "org unit not found");

            // Default-closed access: a non-admin may read this unit only if it
            // is within their membership scope. resolve() scoped to the unit
            // returns it iff readable, and SURFACES a DB error (-> 503) rather
            // than looking like "no access". A no-access hit is 404, not 403,
            // so we do not reveal that the unit exists. Admins may read any unit
            // in their tenant.
            if (!id.is_admin) {
                auto scope = co_await PostgresAccessResolver(tx).resolve(
                    company_id, id.user_id, org_unit_id);
                if (!scope) {
                    const auto code = status_for(scope.error().kind);
                    spdlog::warn("[org_get] scope resolution failed ({}): {}",
                                 static_cast<int>(code), scope.error().message);
                    co_return json_error(code, client_msg(code));
                }
                bool readable = false;
                for (const auto& oid : scope->org_unit_ids)
                    if (oid == org_unit_id) { readable = true; break; }
                if (!readable)
                    co_return json_error(drogon::k404NotFound, "org unit not found");
            }

            const auto& r = orows[0];
            OrgDto dto{
                .id          = r["id"].as<std::string>(),
                .parent_id   = r["parent_id"].isNull()
                                 ? std::nullopt
                                 : std::optional<std::string>(r["parent_id"].as<std::string>()),
                .type        = r["type"].as<std::string>(),
                .slug        = r["slug"].as<std::string>(),
                .name        = r["name"].as<std::string>(),
                .description = r["description"].isNull() ? "" : r["description"].as<std::string>(),
            };

            std::string out;
            if (glz::write<glz::opts{.skip_null_members = false}>(dto, out))
                co_return json_error(drogon::k500InternalServerError, "internal error");
            co_return json(drogon::k200OK, std::move(out));
        } catch (const drogon::orm::DrogonDbException& ex) {
            co_return db_error("org lookup", ex);
        }

    } catch (const std::exception& ex) {
        spdlog::error("[org_get] unhandled exception: {}", ex.what());
        co_return json_error(drogon::k500InternalServerError, "internal error");
    } catch (...) {
        spdlog::error("[org_get] unhandled non-standard exception");
        co_return json_error(drogon::k500InternalServerError, "internal error");
    }
}
