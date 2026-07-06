#pragma once

#include "wikore/auth.hpp"
#include "wikore/domain/types.hpp"                     // RequestContext, Error, uuid_generate
#include "wikore/adapters/postgres/deadline_exec.hpp"  // exec_until
#include "wikore/adapters/postgres/error_mapper.hpp"   // map_db_exception

#include <drogon/HttpResponse.h>
#include <drogon/HttpTypes.h>
#include <drogon/orm/DbClient.h>
#include <drogon/orm/Exception.h>
#include <drogon/utils/coroutine.h>
#include <glaze/glaze.hpp>
#include <spdlog/spdlog.h>

#include <cctype>
#include <chrono>
#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

// ---------------------------------------------------------------------------
// Shared JSON / status / validation helpers for the HTTP handlers under
// apps/api/. Kept as `inline` functions in a header rather than in a
// translation unit so anonymous-namespace symbols in the individual
// handler files don't have to be moved into a public library. The
// symbols have external (inline) linkage — the ODR merge is safe here
// because every definition is textually identical across TUs.
//
// Every helper follows the same contract as the existing wiki_query.cpp:
// log server-side details, return client-safe messages, never leak DB
// error strings past a 5xx boundary.
// ---------------------------------------------------------------------------

namespace wikore::api::http {

// --- error-payload DTO -----------------------------------------------------

struct ErrDto {
    std::string error;
};

// --- response helpers ------------------------------------------------------

inline drogon::HttpResponsePtr
json(drogon::HttpStatusCode code, std::string body)
{
    auto r = drogon::HttpResponse::newHttpResponse();
    r->setStatusCode(code);
    r->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    r->setBody(std::move(body));
    return r;
}

// Build a {"error":"..."} body via glaze so the message is correctly escaped.
inline drogon::HttpResponsePtr
json_error(drogon::HttpStatusCode code, std::string_view msg)
{
    std::string out;
    if (glz::write_json(ErrDto{std::string(msg)}, out)) {
        out = R"({"error":"internal error"})";
    }
    return json(code, std::move(out));
}

// Map a domain Error kind onto an HTTP status. 5xx-class errors carry a
// generic message to the client (the detail is logged, not echoed).
inline drogon::HttpStatusCode status_for(Error::Kind k)
{
    using K = Error::Kind;
    switch (k) {
        case K::InvalidInput:       return drogon::k400BadRequest;
        case K::Forbidden:          return drogon::k403Forbidden;
        case K::NotFound:           return drogon::k404NotFound;
        case K::Conflict:           return drogon::k409Conflict;
        case K::ServiceUnavailable: return drogon::k503ServiceUnavailable;
        case K::InvalidState:
        case K::DatabaseError:      return drogon::k500InternalServerError;
    }
    return drogon::k500InternalServerError;
}

// --- input sanitizers ------------------------------------------------------

// Trim ASCII whitespace from both ends.
inline std::string trim(std::string_view s)
{
    auto b = s.begin(), e = s.end();
    while (b != e && std::isspace(static_cast<unsigned char>(*b)))       ++b;
    while (e != b && std::isspace(static_cast<unsigned char>(*(e - 1)))) --e;
    return std::string(b, e);
}

// Canonical 8-4-4-4-12 lower/upper hex UUID shape. Guards untrusted path
// parameters so a non-UUID never reaches a `::uuid` cast (which would
// otherwise surface as a 500 instead of the correct 404).
inline bool looks_like_uuid(std::string_view s)
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

// --- shared request-prep pipeline ------------------------------------------

// Result of auth + tenant + scope validation. On success carries the fully
// built RequestContext; on failure carries the pre-built HttpResponsePtr
// (401 / 403 / 404 / 500 / 503) that the caller should return as-is.
using PrepResult =
    std::variant<wikore::RequestContext, drogon::HttpResponsePtr>;

// One-shot prep: verify the AuthFilter deposited an identity, look up the
// authenticated user's tenant (re-checking deactivated_at), and verify the
// URL org_unit_id belongs to that tenant. The 30-second whole-request
// deadline is stamped here so the caller reuses the same budget for the
// downstream retrieval + gate work.
//
// The `handler_tag` is included in log lines so a follow-up incident
// review can distinguish which handler emitted a 5xx.
inline drogon::Task<PrepResult>
prep_request(std::string_view                             handler_tag,
             drogon::orm::DbClientPtr                     db,
             drogon::HttpRequestPtr                       req,
             std::string_view                             org_unit_id,
             std::chrono::steady_clock::time_point        deadline)
{
    // 1. Identity. AuthFilter guarantees it; guard defensively.
    if (!req->getAttributes()->find("identity"))
        co_return json_error(drogon::k401Unauthorized, "unauthenticated");
    const auto id = req->getAttributes()->get<Identity>("identity");

    // 2. Reject a malformed scope id before it hits a ::uuid cast.
    if (!looks_like_uuid(org_unit_id))
        co_return json_error(drogon::k404NotFound, "org unit not found");

    // 3. Resolve the tenant from the authenticated user (never the URL).
    //    Re-check deactivated_at here (not just at auth) so a still-cached
    //    credential cannot keep retrieving documents after deactivation.
    std::string company_id;
    try {
        auto rows = co_await postgres::exec_until(
            db, deadline,
            "SELECT company_id FROM users "
            "WHERE id=$1::uuid AND deactivated_at IS NULL", id.user_id);
        if (rows.empty())
            co_return json_error(drogon::k403Forbidden,
                                 "user not found or deactivated");
        company_id = rows[0]["company_id"].as<std::string>();
    } catch (const drogon::orm::DrogonDbException& ex) {
        const auto e    = postgres::map_db_exception(ex);
        const auto code = status_for(e.kind);
        spdlog::error("[{}] tenant lookup failed ({}): {}",
                      handler_tag, static_cast<int>(code), ex.base().what());
        co_return json_error(code,
            code == drogon::k503ServiceUnavailable
                ? "service temporarily unavailable" : "internal error");
    }

    // 4. The scope org unit must belong to that company. A miss returns
    //    404 without distinguishing "absent" from "other tenant" so org
    //    unit existence never leaks across tenants.
    try {
        auto rows = co_await postgres::exec_until(
            db, deadline,
            "SELECT 1 FROM org_units WHERE id=$1::uuid AND company_id=$2::uuid",
            std::string(org_unit_id), company_id);
        if (rows.empty())
            co_return json_error(drogon::k404NotFound, "org unit not found");
    } catch (const drogon::orm::DrogonDbException& ex) {
        const auto e    = postgres::map_db_exception(ex);
        const auto code = status_for(e.kind);
        spdlog::error("[{}] scope check failed ({}): {}",
                      handler_tag, static_cast<int>(code), ex.base().what());
        co_return json_error(code,
            code == drogon::k503ServiceUnavailable
                ? "service temporarily unavailable" : "internal error");
    }

    // 5. Build the request context.
    wikore::RequestContext ctx{
        .tenant    = {.company_id = std::move(company_id)},
        .principal = {.user_id            = id.user_id,
                      .email              = id.email,
                      .display_name       = id.display_name,
                      .is_admin           = id.is_admin,
                      .is_service_account = false},
        .span      = {.trace_id = uuid_generate(),
                      .span_id  = uuid_generate()},
        .deadline  = deadline,
    };
    co_return ctx;
}

// Map a Result-error to a client-safe HttpResponsePtr with the right status
// code and message policy (5xx = generic, 4xx = detail).
inline drogon::HttpResponsePtr
error_response(std::string_view handler_tag, const Error& e)
{
    const auto code = status_for(e.kind);
    spdlog::warn("[{}] retrieval failed ({}): {}",
                 handler_tag, static_cast<int>(code), e.message);
    std::string_view msg =
        code == drogon::k503ServiceUnavailable ? "service temporarily unavailable"
      : code >= drogon::k500InternalServerError ? "internal error"
      : std::string_view(e.message);
    return json_error(code, msg);
}

} // namespace wikore::api::http
