#include "handlers.hpp"

#include "wikore/auth.hpp"           // Identity

#include <drogon/orm/Exception.h>
#include <glaze/glaze.hpp>
#include <spdlog/spdlog.h>

#include <string>
#include <string_view>

using namespace wikore;
using drogon::HttpResponse;
using drogon::HttpResponsePtr;

namespace {

// glaze reflects the member names into JSON keys.
struct MeDto {
    std::string user_id;
    std::string email;
    std::string display_name;
    bool        is_admin = false;
    std::string company_id;
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

} // namespace

drogon::Task<HttpResponsePtr>
wikore::api::me(drogon::orm::DbClientPtr db, drogon::HttpRequestPtr req)
{
    try {
        // AuthFilter guarantees identity; guard defensively.
        if (!req->getAttributes()->find("identity"))
            co_return json_error(drogon::k401Unauthorized, "unauthenticated");
        const auto id = req->getAttributes()->get<Identity>("identity");

        // Tenant from the authenticated user (id.user_id is the internal
        // users.id after AuthFilter resolution). Re-check deactivated_at as
        // defense in depth.
        std::string company_id;
        try {
            auto rows = co_await db->execSqlCoro(
                "SELECT company_id FROM users "
                "WHERE id=$1::uuid AND deactivated_at IS NULL", id.user_id);
            if (rows.empty())
                co_return json_error(drogon::k403Forbidden, "user not found or deactivated");
            company_id = rows[0]["company_id"].as<std::string>();
        } catch (const drogon::orm::DrogonDbException& ex) {
            spdlog::error("[me] tenant lookup failed: {}", ex.base().what());
            co_return json_error(drogon::k500InternalServerError, "internal error");
        }

        std::string out;
        if (glz::write_json(MeDto{id.user_id, id.email, id.display_name,
                                  id.is_admin, company_id}, out))
            co_return json_error(drogon::k500InternalServerError, "internal error");
        co_return json(drogon::k200OK, std::move(out));

    } catch (const std::exception& ex) {
        spdlog::error("[me] unhandled exception: {}", ex.what());
        co_return json_error(drogon::k500InternalServerError, "internal error");
    } catch (...) {
        spdlog::error("[me] unhandled non-standard exception");
        co_return json_error(drogon::k500InternalServerError, "internal error");
    }
}
