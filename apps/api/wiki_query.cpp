#include "handlers.hpp"

#include "wikore/auth.hpp"           // Identity
#include "wikore/domain/types.hpp"   // RequestContext, Error, uuid_generate
#include "wikore/rag/types.hpp"      // AllowedCandidate

#include <drogon/orm/Exception.h>
#include <glaze/glaze.hpp>
#include <spdlog/spdlog.h>

#include <cctype>
#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace wikore;
using drogon::HttpResponse;
using drogon::HttpResponsePtr;

namespace {

// --- wire DTOs (glaze reflects the member names into JSON keys) ------------

struct QueryReq {
    std::string query;
    int         limit = 20;
};

struct ResultDto {
    std::string                chunk_id;
    std::string                document_version_id;
    float                      score = 0.0f;
    std::string                text;
    std::optional<std::string> section_heading;
};

struct QueryResp {
    std::vector<ResultDto> results;
};

struct ErrDto {
    std::string error;
};

// --- response helpers ------------------------------------------------------

HttpResponsePtr json(drogon::HttpStatusCode code, std::string body)
{
    auto r = HttpResponse::newHttpResponse();
    r->setStatusCode(code);
    r->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    r->setBody(std::move(body));
    return r;
}

// Build a {"error":"..."} body via glaze so the message is correctly escaped.
HttpResponsePtr json_error(drogon::HttpStatusCode code, std::string_view msg)
{
    std::string out;
    if (glz::write_json(ErrDto{std::string(msg)}, out)) {
        out = R"({"error":"internal error"})";
    }
    return json(code, std::move(out));
}

// Map a domain Error kind onto an HTTP status. 5xx-class errors carry a
// generic message to the client (the detail is logged, not echoed).
drogon::HttpStatusCode status_for(Error::Kind k)
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

// Trim ASCII whitespace from both ends.
std::string trim(std::string_view s)
{
    auto b = s.begin(), e = s.end();
    while (b != e && std::isspace(static_cast<unsigned char>(*b)))       ++b;
    while (e != b && std::isspace(static_cast<unsigned char>(*(e - 1)))) --e;
    return std::string(b, e);
}

// Canonical 8-4-4-4-12 lower/upper hex UUID shape. Guards the untrusted path
// parameter so a non-UUID never reaches a `::uuid` cast (which would surface
// as a 500 instead of the correct 404).
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
wikore::api::wiki_query(std::shared_ptr<rag::RetrievalOrchestrator> orch,
                        drogon::orm::DbClientPtr                    db,
                        drogon::HttpRequestPtr                      req,
                        std::string                                 org_unit_id)
{
    try {
        // 1. Identity. AuthFilter guarantees it; guard defensively.
        if (!req->getAttributes()->find("identity"))
            co_return json_error(drogon::k401Unauthorized, "unauthenticated");
        const auto id = req->getAttributes()->get<Identity>("identity");

        // 2. Parse + validate the body.
        QueryReq body;
        if (glz::read<glz::opts{.error_on_unknown_keys = false}>(body, req->getBody()))
            co_return json_error(drogon::k400BadRequest, "malformed JSON body");

        const std::string query = trim(body.query);
        if (query.empty())
            co_return json_error(drogon::k400BadRequest, "query must be non-empty");
        if (body.limit <= 0)
            co_return json_error(drogon::k400BadRequest, "limit must be positive");
        const int limit = std::min(body.limit, 100);   // cap abuse

        // 3. Reject a malformed scope id before it hits a ::uuid cast.
        if (!looks_like_uuid(org_unit_id))
            co_return json_error(drogon::k404NotFound, "org unit not found");

        // 4. Resolve the tenant from the authenticated user (never the URL).
        std::string company_id;
        try {
            auto rows = co_await db->execSqlCoro(
                "SELECT company_id FROM users WHERE id=$1::uuid", id.user_id);
            if (rows.empty())
                co_return json_error(drogon::k403Forbidden, "user has no company");
            company_id = rows[0]["company_id"].as<std::string>();
        } catch (const drogon::orm::DrogonDbException& ex) {
            spdlog::error("[wiki_query] tenant lookup failed: {}", ex.base().what());
            co_return json_error(drogon::k500InternalServerError, "internal error");
        }

        // 5. The scope org unit must belong to that company. A miss returns
        //    404 without distinguishing "absent" from "other tenant" so org
        //    unit existence never leaks across tenants.
        try {
            auto rows = co_await db->execSqlCoro(
                "SELECT 1 FROM org_units WHERE id=$1::uuid AND company_id=$2::uuid",
                org_unit_id, company_id);
            if (rows.empty())
                co_return json_error(drogon::k404NotFound, "org unit not found");
        } catch (const drogon::orm::DrogonDbException& ex) {
            spdlog::error("[wiki_query] scope check failed: {}", ex.base().what());
            co_return json_error(drogon::k500InternalServerError, "internal error");
        }

        // 6. Build the request context and retrieve.
        RequestContext ctx{
            .tenant    = {.company_id = company_id},
            .principal = {.user_id            = id.user_id,
                          .email              = id.email,
                          .display_name       = id.display_name,
                          .is_admin           = id.is_admin,
                          .is_service_account = false},
            .span      = {.trace_id = uuid_generate(), .span_id = uuid_generate()},
            .deadline  = std::chrono::steady_clock::now() + std::chrono::seconds(30),
        };

        // NOTE: single-collection retrieval - the orchestrator holds one
        // VectorStorePort (the primary embedding model's collection). Fanning
        // a query across every model's collection is a separate change.
        auto result = co_await orch->retrieve(ctx, query, org_unit_id, limit);
        if (!result) {
            const auto& e    = result.error();
            const auto  code = status_for(e.kind);
            spdlog::warn("[wiki_query] retrieve failed ({}): {}",
                         static_cast<int>(code), e.message);
            std::string_view msg =
                code == drogon::k503ServiceUnavailable ? "service temporarily unavailable"
              : code >= drogon::k500InternalServerError ? "internal error"
              : std::string_view(e.message);
            co_return json_error(code, msg);
        }

        // 7. Serialize. skip_null_members=false so section_heading is emitted
        //    as explicit null when absent (matches the documented contract).
        QueryResp resp;
        resp.results.reserve(result->size());
        for (const auto& c : *result)
            resp.results.push_back(ResultDto{.chunk_id            = c.chunk_id,
                                             .document_version_id = c.document_version_id,
                                             .score               = c.score,
                                             .text                = c.text,
                                             .section_heading     = c.section_heading});

        std::string out;
        if (glz::write<glz::opts{.skip_null_members = false}>(resp, out))
            co_return json_error(drogon::k500InternalServerError, "internal error");
        co_return json(drogon::k200OK, std::move(out));

    } catch (const std::exception& ex) {
        spdlog::error("[wiki_query] unhandled exception: {}", ex.what());
        co_return json_error(drogon::k500InternalServerError, "internal error");
    } catch (...) {
        spdlog::error("[wiki_query] unhandled non-standard exception");
        co_return json_error(drogon::k500InternalServerError, "internal error");
    }
}
