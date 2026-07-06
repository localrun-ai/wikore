#include "handlers.hpp"
#include "http_helpers.hpp"

#include "wikore/domain/types.hpp"
#include "wikore/rag/answer_use_case.hpp"

#include <glaze/glaze.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// POST /api/orgs/{orgUnitId}/wiki/answer — grounded LLM answer with
// citations (BaryGraph Lite step 8c).
//
// End-to-end path: retrieve_evidence → ContextBuilder → LlmProvider.
// The system prompt supplied by AnswerUseCase spells out the origin
// vocabulary (parser / deterministic_rule / administrator / llm_proposal)
// so the LLM can distinguish confirmed edges from model proposals when
// composing the answer.
//
// Answer citation validation (rejecting hallucinated [SRC N] / [REL N]
// ids that the model made up) is a follow-up (docs step 10:
// AnswerFinalizer). This endpoint reports the ALLOWED citation IDs the
// prompt exposed so a caller-side validator can be layered without
// changing the wire shape.
//
// Request body shape (a superset of /wiki/evidence):
//   {
//     "query": "...",
//     "limit": 10,
//     "intent": "fact" | "bridge" | "automatic",     // default "automatic"
//     "min_confidence": 0.6,                          // optional
//     "allowed_review_states": ["accepted"],          // optional
//     "model": "provider-specific-name",              // optional
//     "system_prompt": "override text"                // optional
//   }
//
// Response body:
//   {
//     "answer": "...",
//     "citations": {
//       "source_chunk_ids": ["...","..."],
//       "source_edge_ids":  ["..."]
//     },
//     "model": "gpt-4o-mini",
//     "usage": {"input_tokens": 512, "output_tokens": 128},
//     "evidence_included": 3
//   }
// ---------------------------------------------------------------------------

using namespace wikore;
using drogon::HttpResponsePtr;

namespace {

// --- request DTO -----------------------------------------------------------

struct AnswerReq {
    std::string                             query;
    int                                     limit = 10;
    std::string                             intent = "automatic";
    std::optional<double>                   min_confidence;
    std::optional<std::vector<std::string>> allowed_review_states;
    std::optional<std::string>              model;
    std::optional<std::string>              system_prompt;
};

// --- response DTOs ---------------------------------------------------------

struct CitationsDto {
    std::vector<std::string> source_chunk_ids;
    std::vector<std::string> source_edge_ids;
};

struct UsageDto {
    int input_tokens  = 0;
    int output_tokens = 0;
};

struct AnswerResp {
    std::string  answer;
    CitationsDto citations;
    std::string  model;
    UsageDto     usage;
    int          evidence_included = 0;
};

std::optional<rag::RetrievalIntent> parse_intent(std::string_view s)
{
    if (s == "fact")      return rag::RetrievalIntent::Fact;
    if (s == "bridge")    return rag::RetrievalIntent::Bridge;
    if (s == "automatic") return rag::RetrievalIntent::Automatic;
    return std::nullopt;
}

} // namespace

drogon::Task<HttpResponsePtr>
wikore::api::wiki_answer(std::shared_ptr<rag::AnswerUseCase> use_case,
                         drogon::orm::DbClientPtr            db,
                         drogon::HttpRequestPtr              req,
                         std::string                         org_unit_id)
{
    using namespace wikore::api::http;
    try {
        // 30s whole-request budget; the LLM call takes the remaining time
        // as its per-call cap (see AnswerUseCase::run).
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(30);

        AnswerReq body;
        if (glz::read<glz::opts{.error_on_unknown_keys = false}>(body, req->getBody()))
            co_return json_error(drogon::k400BadRequest, "malformed JSON body");

        const std::string query = trim(body.query);
        if (query.empty())
            co_return json_error(drogon::k400BadRequest, "query must be non-empty");
        if (body.limit <= 0)
            co_return json_error(drogon::k400BadRequest, "limit must be positive");
        const int limit = std::min(body.limit, 50);   // tighter cap than
                                                       // /wiki/evidence — the
                                                       // LLM prompt has a
                                                       // real cost per item.

        const auto intent_opt = parse_intent(body.intent);
        if (!intent_opt) {
            spdlog::warn("[wiki_answer] unknown intent: '{}'", body.intent);
            co_return json_error(drogon::k400BadRequest,
                "intent must be one of: fact, bridge, automatic");
        }

        rag::AnswerOptions opts;
        opts.intent = *intent_opt;
        opts.limit  = limit;
        if (body.min_confidence) {
            if (*body.min_confidence < 0.0 || *body.min_confidence > 1.0)
                co_return json_error(drogon::k400BadRequest,
                    "min_confidence must be in [0.0, 1.0]");
            opts.bridge_opts.min_confidence = *body.min_confidence;
        }
        if (body.allowed_review_states) {
            if (body.allowed_review_states->empty())
                co_return json_error(drogon::k400BadRequest,
                    "allowed_review_states, if present, must be non-empty");
            opts.bridge_opts.allowed_review_states = *body.allowed_review_states;
        }
        if (body.model && !body.model->empty())
            opts.model = *body.model;
        // Three-state override: absent = default system prompt;
        // present-but-empty = genuinely no system message (eval
        // harness); non-empty = used verbatim. Pass the optional
        // through directly so we distinguish absent from empty.
        if (body.system_prompt.has_value())
            opts.system_prompt_override = *body.system_prompt;

        auto prep = co_await prep_request(
            "wiki_answer", db, req, org_unit_id, deadline);
        if (auto* err = std::get_if<HttpResponsePtr>(&prep))
            co_return *err;
        const auto& ctx = std::get<RequestContext>(prep);

        auto result = co_await use_case->run(ctx, query, org_unit_id, opts);
        if (!result)
            co_return error_response("wiki_answer", result.error());

        AnswerResp resp{
            .answer    = std::move(result->answer),
            .citations = CitationsDto{
                .source_chunk_ids = std::move(result->source_chunk_ids),
                .source_edge_ids  = std::move(result->source_edge_ids),
            },
            .model     = std::move(result->model),
            .usage     = UsageDto{
                .input_tokens  = result->input_tokens,
                .output_tokens = result->output_tokens,
            },
            .evidence_included = result->evidence_included,
        };

        std::string out;
        if (glz::write<glz::opts{.skip_null_members = false}>(resp, out))
            co_return json_error(drogon::k500InternalServerError, "internal error");
        co_return json(drogon::k200OK, std::move(out));

    } catch (const std::exception& ex) {
        spdlog::error("[wiki_answer] unhandled exception: {}", ex.what());
        co_return json_error(drogon::k500InternalServerError, "internal error");
    } catch (...) {
        spdlog::error("[wiki_answer] unhandled non-standard exception");
        co_return json_error(drogon::k500InternalServerError, "internal error");
    }
}
