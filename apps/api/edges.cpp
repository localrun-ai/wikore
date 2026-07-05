#include "handlers.hpp"
#include "wikore/auth.hpp"
#include "wikore/domain/knowledge_edge.hpp"
#include "wikore/rag/knowledge_edge_repo.hpp"

#include <glaze/glaze.hpp>
#include <spdlog/spdlog.h>

#include <optional>
#include <string>
#include <string_view>

using namespace wikore;
using drogon::HttpResponse;
using drogon::HttpResponsePtr;

namespace {

// ---------------------------------------------------------------------------
// Wire DTOs. Kept intentionally close to the on-the-wire JSON shape (glaze
// serialises struct fields by name); domain::* enums are round-tripped as
// their canonical wire strings via to_wire()/parse_*.
// ---------------------------------------------------------------------------

struct EndpointDto {
    int         ordinal;
    std::string chunk_id;
    std::string role;
};

struct EdgeDto {
    std::string                              id;
    std::string                              company_id;
    std::string                              edge_type;
    std::string                              direction;
    double                                   confidence;
    std::string                              origin;
    std::string                              review_state;
    // provenance is returned as the RAW JSONB text (e.g. `{"parser":"foo"}`),
    // NOT as a nested object. That is deliberate: glaze would otherwise have
    // to parse arbitrary user-controlled JSON at serialisation time. Clients
    // must JSON.parse() the string themselves.
    std::string                              provenance;
    int                                      formula_version;
    long long                                edge_version;
    std::optional<std::string>               created_by;
    std::optional<std::string>               reviewed_by;
    std::string                              created_at;
    std::optional<std::string>               reviewed_at;
    std::optional<std::string>               expires_at;
    std::optional<std::string>               superseded_at;
    std::vector<EndpointDto>                 endpoints;
};

struct ListResponseDto {
    std::vector<EdgeDto> edges;
};

struct ErrDto {
    std::string error;
};

// Wire → JSON. Every field is present in the response (skip_null_members=false)
// so clients can rely on stable shape; optional values render as `null`.
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
        case Error::Kind::NotFound:           return drogon::k404NotFound;
        case Error::Kind::Conflict:           return drogon::k409Conflict;
        case Error::Kind::Forbidden:          return drogon::k403Forbidden;
        case Error::Kind::InvalidInput:       return drogon::k400BadRequest;
        case Error::Kind::InvalidState:       return drogon::k422UnprocessableEntity;
        case Error::Kind::ServiceUnavailable: return drogon::k503ServiceUnavailable;
        default:                              return drogon::k500InternalServerError;
    }
}

std::string_view client_msg(drogon::HttpStatusCode code)
{
    return code == drogon::k503ServiceUnavailable ? "service temporarily unavailable"
         : code >= drogon::k500InternalServerError ? "internal error"
         : "request could not be completed";
}

EdgeDto to_dto(const domain::KnowledgeEdge& e)
{
    EdgeDto d{
        .id              = e.id,
        .company_id      = e.company_id,
        .edge_type       = std::string(domain::to_wire(e.edge_type)),
        .direction       = std::string(domain::to_wire(e.direction)),
        .confidence      = e.confidence,
        .origin          = std::string(domain::to_wire(e.origin)),
        .review_state    = std::string(domain::to_wire(e.review_state)),
        .provenance      = e.provenance_json,
        .formula_version = e.formula_version,
        .edge_version    = e.edge_version,
        .created_by      = e.created_by,
        .reviewed_by     = e.reviewed_by,
        .created_at      = e.created_at,
        .reviewed_at     = e.reviewed_at,
        .expires_at      = e.expires_at,
        .superseded_at   = e.superseded_at,
        .endpoints       = {},
    };
    d.endpoints.reserve(e.endpoints.size());
    for (const auto& ep : e.endpoints) {
        d.endpoints.push_back({
            .ordinal  = ep.ordinal,
            .chunk_id = ep.chunk_id,
            .role     = std::string(domain::to_wire(ep.role)),
        });
    }
    return d;
}

HttpResponsePtr render_edge(drogon::HttpStatusCode code, const domain::KnowledgeEdge& e)
{
    std::string out;
    if (glz::write<glz::opts{.skip_null_members = false}>(to_dto(e), out))
        return json_error(drogon::k500InternalServerError, "internal error");
    return json(code, std::move(out));
}

// -------- create request --------
struct CreateEndpointReq {
    int         ordinal;
    std::string chunk_id;
    std::string role;
};

struct CreateEdgeReq {
    std::string                edge_type;
    std::string                direction;
    double                     confidence   = 0.0;
    std::string                origin;
    std::optional<std::string> review_state;
    std::optional<std::string> provenance;    // raw JSONB (must be a JSON object literal)
    std::optional<std::string> expires_at;
    CreateEndpointReq          endpoint_0;
    CreateEndpointReq          endpoint_1;
};

// -------- update request --------
struct UpdateEdgeReq {
    std::optional<double>      confidence;
    std::optional<std::string> review_state;
    std::optional<std::string> provenance;
    std::optional<std::string> expires_at;   // "" clears
};

// Resolve tenant + verify caller is admin. Returns {company_id, user_id}.
struct AdminCtx {
    std::string company_id;
    std::string user_id;
};

drogon::Task<Result<AdminCtx>>
resolve_admin(drogon::orm::DbClientPtr db, const drogon::HttpRequestPtr& req)
{
    // AuthFilter guarantees identity on any route it fronts. The other
    // handlers (org_get, me) render this defensive branch as 401
    // Unauthorized, but Error::Kind has no Unauthorized variant, so we
    // return forbidden() here — Kind::Forbidden -> 403. The branch is
    // effectively unreachable; a stray 403 for a missing identity is
    // still closer to correct than a 500, and if this ever fires it is
    // a wiring bug (AuthFilter missing on a new admin route) that we
    // want surfaced.
    if (!req->getAttributes()->find("identity"))
        co_return std::unexpected(Error::forbidden("unauthenticated"));
    const auto id = req->getAttributes()->get<Identity>("identity");
    // AdminFilter already gates the route but double-check as defense in
    // depth (the filter is easy to forget on new routes).
    if (!id.is_admin)
        co_return std::unexpected(Error::forbidden("admin only"));
    try {
        auto rows = co_await db->execSqlCoro(
            "SELECT company_id::text AS company_id FROM users "
            "WHERE id = $1::uuid AND deactivated_at IS NULL",
            id.user_id);
        if (rows.empty())
            co_return std::unexpected(Error::forbidden("user not found or deactivated"));
        co_return AdminCtx{
            .company_id = rows[0]["company_id"].as<std::string>(),
            .user_id    = id.user_id,
        };
    } catch (const drogon::orm::DrogonDbException& ex) {
        spdlog::error("[edges] admin resolve failed: {}", ex.base().what());
        co_return std::unexpected(Error::unavailable("service temporarily unavailable"));
    }
}

// Parse a "limit"/"offset" pair from the query string. Enforces sane bounds
// so a caller can't request 1e9 rows.
struct Page { int limit = 50; int offset = 0; };

Result<Page> parse_page(const drogon::HttpRequestPtr& req)
{
    Page p;
    if (auto v = req->getParameter("limit"); !v.empty()) {
        try { p.limit = std::stoi(v); }
        catch (...) { return std::unexpected(Error::invalid_input("limit is not an integer")); }
        if (p.limit <= 0 || p.limit > 200)
            return std::unexpected(Error::invalid_input("limit must be in [1, 200]"));
    }
    if (auto v = req->getParameter("offset"); !v.empty()) {
        try { p.offset = std::stoi(v); }
        catch (...) { return std::unexpected(Error::invalid_input("offset is not an integer")); }
        if (p.offset < 0)
            return std::unexpected(Error::invalid_input("offset must be >= 0"));
    }
    return p;
}

} // namespace

// ---------------------------------------------------------------------------
// POST /api/admin/edges
// ---------------------------------------------------------------------------

drogon::Task<HttpResponsePtr>
wikore::api::edges_create(std::shared_ptr<rag::KnowledgeEdgeRepo> repo,
                          drogon::orm::DbClientPtr                db,
                          drogon::HttpRequestPtr                  req)
{
    try {
        auto ctx = co_await resolve_admin(db, req);
        if (!ctx) {
            const auto code = status_for(ctx.error().kind);
            co_return json_error(code, ctx.error().message);
        }

        // Parse body.
        CreateEdgeReq body;
        if (auto err = glz::read_json(body, std::string_view(req->body())); err) {
            co_return json_error(drogon::k400BadRequest,
                                 std::format("invalid JSON: {}", glz::format_error(err, req->body())));
        }

        auto etype = domain::parse_edge_type(body.edge_type);
        if (!etype)
            co_return json_error(drogon::k400BadRequest, "edge_type is not one of the V1 vocabulary");
        auto dir = domain::parse_direction(body.direction);
        if (!dir)
            co_return json_error(drogon::k400BadRequest, "direction must be 'directed' or 'symmetric'");
        auto origin = domain::parse_origin(body.origin);
        if (!origin)
            co_return json_error(drogon::k400BadRequest, "origin is not one of the accepted values");
        auto role0 = domain::parse_role(body.endpoint_0.role);
        auto role1 = domain::parse_role(body.endpoint_1.role);
        if (!role0 || !role1)
            co_return json_error(drogon::k400BadRequest, "endpoint role is not one of the accepted values");
        std::optional<domain::EdgeReviewState> review;
        if (body.review_state) {
            review = domain::parse_review_state(*body.review_state);
            if (!review)
                co_return json_error(drogon::k400BadRequest, "review_state is not one of the accepted values");
        }

        domain::CreateKnowledgeEdgeCmd cmd{
            .edge_type       = *etype,
            .direction       = *dir,
            .confidence      = body.confidence,
            .origin          = *origin,
            .review_state    = review,
            .provenance_json = body.provenance.value_or("{}"),
            .expires_at      = body.expires_at,
            .endpoint_0      = {.ordinal = body.endpoint_0.ordinal,
                                .chunk_id = body.endpoint_0.chunk_id,
                                .role     = *role0},
            .endpoint_1      = {.ordinal = body.endpoint_1.ordinal,
                                .chunk_id = body.endpoint_1.chunk_id,
                                .role     = *role1},
        };

        auto r = co_await repo->create(ctx->company_id, ctx->user_id, cmd);
        if (!r) {
            const auto code = status_for(r.error().kind);
            spdlog::warn("[edges_create] {} ({})", r.error().message, static_cast<int>(code));
            co_return json_error(code, code >= drogon::k500InternalServerError
                                           ? client_msg(code)
                                           : r.error().message);
        }
        co_return render_edge(drogon::k201Created, *r);
    } catch (const std::exception& ex) {
        spdlog::error("[edges_create] unhandled: {}", ex.what());
        co_return json_error(drogon::k500InternalServerError, "internal error");
    }
}

// ---------------------------------------------------------------------------
// GET /api/admin/edges/{edge_id}
// ---------------------------------------------------------------------------

drogon::Task<HttpResponsePtr>
wikore::api::edges_get(std::shared_ptr<rag::KnowledgeEdgeRepo> repo,
                       drogon::orm::DbClientPtr                db,
                       drogon::HttpRequestPtr                  req,
                       std::string                             edge_id)
{
    try {
        auto ctx = co_await resolve_admin(db, req);
        if (!ctx) {
            const auto code = status_for(ctx.error().kind);
            co_return json_error(code, ctx.error().message);
        }
        auto r = co_await repo->get(ctx->company_id, edge_id);
        if (!r) {
            const auto code = status_for(r.error().kind);
            co_return json_error(code, code >= drogon::k500InternalServerError
                                           ? client_msg(code)
                                           : r.error().message);
        }
        co_return render_edge(drogon::k200OK, *r);
    } catch (const std::exception& ex) {
        spdlog::error("[edges_get] unhandled: {}", ex.what());
        co_return json_error(drogon::k500InternalServerError, "internal error");
    }
}

// ---------------------------------------------------------------------------
// GET /api/admin/edges?edge_type=...&review_state=...&chunk_id=...&limit=&offset=
// ---------------------------------------------------------------------------

drogon::Task<HttpResponsePtr>
wikore::api::edges_list(std::shared_ptr<rag::KnowledgeEdgeRepo> repo,
                        drogon::orm::DbClientPtr                db,
                        drogon::HttpRequestPtr                  req)
{
    try {
        auto ctx = co_await resolve_admin(db, req);
        if (!ctx) {
            const auto code = status_for(ctx.error().kind);
            co_return json_error(code, ctx.error().message);
        }

        auto page = parse_page(req);
        if (!page) {
            co_return json_error(drogon::k400BadRequest, page.error().message);
        }

        domain::ListKnowledgeEdgesFilter filter;
        filter.limit  = page->limit;
        filter.offset = page->offset;
        if (auto v = req->getParameter("edge_type"); !v.empty()) {
            auto p = domain::parse_edge_type(v);
            if (!p) co_return json_error(drogon::k400BadRequest, "edge_type is not one of the V1 vocabulary");
            filter.edge_type = p;
        }
        if (auto v = req->getParameter("review_state"); !v.empty()) {
            auto p = domain::parse_review_state(v);
            if (!p) co_return json_error(drogon::k400BadRequest, "review_state is not one of the accepted values");
            filter.review_state = p;
        }
        if (auto v = req->getParameter("chunk_id"); !v.empty()) {
            if (!domain::looks_like_uuid(v))
                co_return json_error(drogon::k400BadRequest, "chunk_id must be a UUID");
            filter.endpoint_chunk_id = std::string(v);
        }

        auto r = co_await repo->list(ctx->company_id, filter);
        if (!r) {
            const auto code = status_for(r.error().kind);
            co_return json_error(code, code >= drogon::k500InternalServerError
                                           ? client_msg(code)
                                           : r.error().message);
        }
        ListResponseDto resp;
        resp.edges.reserve(r->size());
        for (const auto& e : *r) resp.edges.push_back(to_dto(e));
        std::string out;
        if (glz::write<glz::opts{.skip_null_members = false}>(resp, out))
            co_return json_error(drogon::k500InternalServerError, "internal error");
        co_return json(drogon::k200OK, std::move(out));
    } catch (const std::exception& ex) {
        spdlog::error("[edges_list] unhandled: {}", ex.what());
        co_return json_error(drogon::k500InternalServerError, "internal error");
    }
}

// ---------------------------------------------------------------------------
// PATCH /api/admin/edges/{edge_id}
// ---------------------------------------------------------------------------

drogon::Task<HttpResponsePtr>
wikore::api::edges_update(std::shared_ptr<rag::KnowledgeEdgeRepo> repo,
                          drogon::orm::DbClientPtr                db,
                          drogon::HttpRequestPtr                  req,
                          std::string                             edge_id)
{
    try {
        auto ctx = co_await resolve_admin(db, req);
        if (!ctx) {
            const auto code = status_for(ctx.error().kind);
            co_return json_error(code, ctx.error().message);
        }

        UpdateEdgeReq body;
        if (auto err = glz::read_json(body, std::string_view(req->body())); err) {
            co_return json_error(drogon::k400BadRequest,
                                 std::format("invalid JSON: {}", glz::format_error(err, req->body())));
        }

        domain::UpdateKnowledgeEdgeCmd cmd;
        cmd.confidence      = body.confidence;
        cmd.provenance_json = body.provenance;
        cmd.expires_at      = body.expires_at;
        if (body.review_state) {
            auto p = domain::parse_review_state(*body.review_state);
            if (!p) co_return json_error(drogon::k400BadRequest,
                                         "review_state is not one of the accepted values");
            cmd.review_state = p;
        }

        auto r = co_await repo->update(ctx->company_id, ctx->user_id, edge_id, cmd);
        if (!r) {
            const auto code = status_for(r.error().kind);
            co_return json_error(code, code >= drogon::k500InternalServerError
                                           ? client_msg(code)
                                           : r.error().message);
        }
        co_return render_edge(drogon::k200OK, *r);
    } catch (const std::exception& ex) {
        spdlog::error("[edges_update] unhandled: {}", ex.what());
        co_return json_error(drogon::k500InternalServerError, "internal error");
    }
}

// ---------------------------------------------------------------------------
// DELETE /api/admin/edges/{edge_id}
// ---------------------------------------------------------------------------

drogon::Task<HttpResponsePtr>
wikore::api::edges_delete(std::shared_ptr<rag::KnowledgeEdgeRepo> repo,
                          drogon::orm::DbClientPtr                db,
                          drogon::HttpRequestPtr                  req,
                          std::string                             edge_id)
{
    try {
        auto ctx = co_await resolve_admin(db, req);
        if (!ctx) {
            const auto code = status_for(ctx.error().kind);
            co_return json_error(code, ctx.error().message);
        }
        auto r = co_await repo->remove(ctx->company_id, edge_id);
        if (!r) {
            const auto code = status_for(r.error().kind);
            co_return json_error(code, code >= drogon::k500InternalServerError
                                           ? client_msg(code)
                                           : r.error().message);
        }
        auto resp = HttpResponse::newHttpResponse();
        resp->setStatusCode(drogon::k204NoContent);
        co_return resp;
    } catch (const std::exception& ex) {
        spdlog::error("[edges_delete] unhandled: {}", ex.what());
        co_return json_error(drogon::k500InternalServerError, "internal error");
    }
}
