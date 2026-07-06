#include "handlers.hpp"
#include "http_helpers.hpp"

#include "wikore/domain/types.hpp"
#include "wikore/rag/types.hpp"                 // AllowedChunk / AllowedRelationship
#include "wikore/rag/retrieval_orchestrator.hpp"

#include <glaze/glaze.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <variant>
#include <vector>

// ---------------------------------------------------------------------------
// POST /api/orgs/{orgUnitId}/wiki/evidence — the intent-dispatched retrieval
// endpoint (BaryGraph Lite step 8b). Backward-compat wrapper around
// RetrievalOrchestrator::retrieve_evidence.
//
// Kept as a sibling of wiki_query rather than merged into it because:
//   * Response shape is a discriminated union ("kind": "chunk" | "relationship")
//     — existing wiki_query clients would break if the shape changed under
//     them.
//   * Bridge and Automatic intents can fail closed / degrade even when the
//     tenant has no edge collection wired up; wiki_query's clients would be
//     surprised by a 503 on a request that used to work.
//
// Request body shape:
//   {
//     "query": "...",
//     "limit": 20,
//     "intent": "fact" | "bridge" | "automatic",     // default "fact"
//     "min_confidence": 0.6,                          // optional
//     "allowed_review_states": ["accepted"]           // optional
//   }
//
// Response body shape:
//   {
//     "results": [
//       {"kind":"chunk", "chunk_id":"...", "document_version_id":"...",
//        "score":0.9, "text":"...", "section_heading":null},
//       {"kind":"relationship", "edge_id":"...", "edge_type":"...",
//        "direction":"directed", "origin":"administrator",
//        "review_state":"accepted", "confidence":0.9, "score":0.9,
//        "edge_version":1,
//        "endpoint_0":{"ordinal":0,"role":"source",...},
//        "endpoint_1":{"ordinal":1,"role":"target",...}}
//     ]
//   }
// ---------------------------------------------------------------------------

using namespace wikore;
using drogon::HttpResponsePtr;

namespace {

// --- wire DTOs -------------------------------------------------------------

struct EvidenceReq {
    std::string              query;
    int                      limit = 20;
    std::string              intent = "fact";
    // Bridge / Automatic tuning knobs. Both optional; empty defaults are
    // materialized to BridgeIntentOptions{} inside the handler (which is
    // {0.6, {"accepted"}}) so absent-in-body means "use the design doc
    // defaults", not "no filter".
    std::optional<double>                  min_confidence;
    std::optional<std::vector<std::string>> allowed_review_states;
};

struct EndpointDto {
    int                        ordinal = 0;
    std::string                role;
    std::string                chunk_id;
    std::string                document_version_id;
    std::string                text;
    std::optional<std::string> section_heading;
};

struct ChunkResultDto {
    std::string                kind = "chunk";      // discriminator
    std::string                chunk_id;
    std::string                document_version_id;
    float                      score = 0.0f;
    std::string                text;
    std::optional<std::string> section_heading;
};

struct RelationshipResultDto {
    std::string  kind = "relationship";             // discriminator
    std::string  edge_id;
    std::string  edge_type;
    std::string  direction;
    std::string  origin;
    std::string  review_state;
    float        score        = 0.0f;
    double       confidence   = 0.0;
    std::int64_t edge_version = 0;
    EndpointDto  endpoint_0;
    EndpointDto  endpoint_1;
};

// Wire discriminated-union item. Glaze's variant serializer would embed a
// type tag; we want an explicit "kind" field per doc §"Context construction"
// callers, so we serialize each alternative directly and hand-roll the
// concatenation.
using EvidenceItem = std::variant<ChunkResultDto, RelationshipResultDto>;

struct EvidenceResp {
    std::vector<EvidenceItem> results;
};

// Parse intent string into RetrievalIntent. Returns nullopt on unknown value
// so the caller emits a 400 with the offending string in the log.
std::optional<rag::RetrievalIntent> parse_intent(std::string_view s)
{
    if (s == "fact")      return rag::RetrievalIntent::Fact;
    if (s == "bridge")    return rag::RetrievalIntent::Bridge;
    if (s == "automatic") return rag::RetrievalIntent::Automatic;
    return std::nullopt;
}

EndpointDto to_dto(const rag::AllowedRelationship::AllowedEndpoint& ep)
{
    return EndpointDto{
        .ordinal              = ep.ordinal,
        .role                 = ep.role,
        .chunk_id             = ep.chunk_id,
        .document_version_id  = ep.document_version_id,
        .text                 = ep.text,
        .section_heading      = ep.section_heading,
    };
}

ChunkResultDto to_dto(const rag::AllowedChunk& c)
{
    return ChunkResultDto{
        .chunk_id            = c.chunk_id(),
        .document_version_id = c.document_version_id(),
        .score               = c.score(),
        .text                = c.text(),
        .section_heading     = c.section_heading(),
    };
}

RelationshipResultDto to_dto(const rag::AllowedRelationship& r)
{
    return RelationshipResultDto{
        .edge_id      = r.edge_id(),
        .edge_type    = r.edge_type(),
        .direction    = r.direction(),
        .origin       = r.origin(),
        .review_state = r.review_state(),
        .score        = r.score(),
        .confidence   = r.confidence(),
        .edge_version = r.edge_version(),
        .endpoint_0   = to_dto(r.endpoint_0()),
        .endpoint_1   = to_dto(r.endpoint_1()),
    };
}

// Assemble the response JSON manually to control the "kind" discrimination.
// Each alternative is glz::write_json'd into a per-item buffer, then joined
// with commas inside a "results": [...] envelope.
std::string serialize_response(const EvidenceResp& resp)
{
    std::string out = R"({"results":[)";
    bool first = true;
    for (const auto& item : resp.results) {
        if (!first) out += ',';
        first = false;
        std::string one;
        const bool err = std::visit([&](const auto& d) {
            return static_cast<bool>(
                glz::write<glz::opts{.skip_null_members = false}>(d, one));
        }, item);
        if (err)
            return {};   // sentinel: caller returns 500
        out += one;
    }
    out += "]}";
    return out;
}

} // namespace

drogon::Task<HttpResponsePtr>
wikore::api::wiki_evidence(std::shared_ptr<rag::RetrievalOrchestrator> orch,
                           drogon::orm::DbClientPtr                    db,
                           drogon::HttpRequestPtr                      req,
                           std::string                                 org_unit_id)
{
    using namespace wikore::api::http;
    try {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(30);

        // Parse + validate the body.
        EvidenceReq body;
        if (glz::read<glz::opts{.error_on_unknown_keys = false}>(body, req->getBody()))
            co_return json_error(drogon::k400BadRequest, "malformed JSON body");

        const std::string query = trim(body.query);
        if (query.empty())
            co_return json_error(drogon::k400BadRequest, "query must be non-empty");
        if (body.limit <= 0)
            co_return json_error(drogon::k400BadRequest, "limit must be positive");
        const int limit = std::min(body.limit, 100);

        const auto intent_opt = parse_intent(body.intent);
        if (!intent_opt) {
            spdlog::warn("[wiki_evidence] unknown intent value: '{}'", body.intent);
            co_return json_error(drogon::k400BadRequest,
                "intent must be one of: fact, bridge, automatic");
        }
        const auto intent = *intent_opt;

        // Build BridgeIntentOptions. If the caller left the fields out we
        // use the struct's defaults (0.6 / {"accepted"}) — those are the
        // documented bridge-intent defaults from the design doc. If the
        // caller sent an empty allowed_review_states list, treat it as
        // "no valid review state" and reject at the HTTP boundary rather
        // than pass it through and get a 400 from retrieve_evidence with
        // a less informative message.
        rag::BridgeIntentOptions bridge_opts;
        if (body.min_confidence) {
            if (*body.min_confidence < 0.0 || *body.min_confidence > 1.0)
                co_return json_error(drogon::k400BadRequest,
                    "min_confidence must be in [0.0, 1.0]");
            bridge_opts.min_confidence = *body.min_confidence;
        }
        if (body.allowed_review_states) {
            if (body.allowed_review_states->empty())
                co_return json_error(drogon::k400BadRequest,
                    "allowed_review_states, if present, must be non-empty");
            bridge_opts.allowed_review_states = *body.allowed_review_states;
        }

        auto prep = co_await prep_request(
            "wiki_evidence", db, req, org_unit_id, deadline);
        if (auto* err = std::get_if<HttpResponsePtr>(&prep))
            co_return *err;
        const auto& ctx = std::get<RequestContext>(prep);

        auto result = co_await orch->retrieve_evidence(
            ctx, query, org_unit_id, intent, bridge_opts, limit);
        if (!result)
            co_return error_response("wiki_evidence", result.error());

        EvidenceResp resp;
        resp.results.reserve(result->size());
        for (const auto& ev : *result) {
            std::visit([&](const auto& allowed) {
                resp.results.emplace_back(to_dto(allowed));
            }, ev);
        }

        auto out = serialize_response(resp);
        if (out.empty())
            co_return json_error(drogon::k500InternalServerError, "internal error");
        co_return json(drogon::k200OK, std::move(out));

    } catch (const std::exception& ex) {
        spdlog::error("[wiki_evidence] unhandled exception: {}", ex.what());
        co_return json_error(drogon::k500InternalServerError, "internal error");
    } catch (...) {
        spdlog::error("[wiki_evidence] unhandled non-standard exception");
        co_return json_error(drogon::k500InternalServerError, "internal error");
    }
}
