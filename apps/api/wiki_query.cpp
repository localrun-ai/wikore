#include "handlers.hpp"
#include "http_helpers.hpp"

#include "wikore/domain/types.hpp"   // RequestContext, Error
#include "wikore/rag/types.hpp"      // AllowedChunk

#include <glaze/glaze.hpp>
#include <spdlog/spdlog.h>

#include <chrono>
#include <optional>
#include <string>
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

} // namespace

drogon::Task<HttpResponsePtr>
wikore::api::wiki_query(std::shared_ptr<rag::RetrievalOrchestrator> orch,
                        drogon::orm::DbClientPtr                    db,
                        drogon::HttpRequestPtr                      req,
                        std::string                                 org_unit_id)
{
    using namespace wikore::api::http;
    try {
        // The 30s budget covers the WHOLE request, so it is stamped before the
        // first DB touch (tenant + org-unit lookups included) and reused for the
        // RequestContext. Every DB step below runs through postgres::exec_until,
        // which caps each query's statement_timeout at the time still remaining.
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(30);

        // Parse + validate the body.
        QueryReq body;
        if (glz::read<glz::opts{.error_on_unknown_keys = false}>(body, req->getBody()))
            co_return json_error(drogon::k400BadRequest, "malformed JSON body");

        const std::string query = trim(body.query);
        if (query.empty())
            co_return json_error(drogon::k400BadRequest, "query must be non-empty");
        if (body.limit <= 0)
            co_return json_error(drogon::k400BadRequest, "limit must be positive");
        const int limit = std::min(body.limit, 100);   // cap abuse

        // Auth + tenant + scope, all with the shared budget.
        auto prep = co_await prep_request(
            "wiki_query", db, req, org_unit_id, deadline);
        if (auto* err = std::get_if<HttpResponsePtr>(&prep))
            co_return *err;
        const auto& ctx = std::get<RequestContext>(prep);

        // NOTE: single-collection retrieval - the orchestrator holds one
        // VectorStorePort (the primary embedding model's collection). Fanning
        // a query across every model's collection is a separate change.
        auto result = co_await orch->retrieve(ctx, query, org_unit_id, limit);
        if (!result)
            co_return error_response("wiki_query", result.error());

        // Serialize. skip_null_members=false so section_heading is emitted
        // as explicit null when absent (matches the documented contract).
        QueryResp resp;
        resp.results.reserve(result->size());
        for (const auto& c : *result)
            resp.results.push_back(ResultDto{.chunk_id            = c.chunk_id(),
                                             .document_version_id = c.document_version_id(),
                                             .score               = c.score(),
                                             .text                = c.text(),
                                             .section_heading     = c.section_heading()});

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
