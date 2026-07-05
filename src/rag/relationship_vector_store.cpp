#include "wikore/rag/relationship_vector_store.hpp"
#include <drogon/HttpClient.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/drogon.h>
#include <glaze/glaze.hpp>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <format>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace wikore::rag {

namespace {

// Same rationale as the retriever's response-shape structs: Qdrant wraps
// every reply as {"result": ..., "status": "ok", "time": ...}. Glaze
// strict-parse would fail on the envelope, so callers opt out of
// error_on_unknown_keys and this struct only names what we consume.
// Namespace scope required — glaze reflection can't handle types defined
// inside a coroutine frame.
struct EdgePayload {
    std::string  company_id;
    std::string  edge_id;
    std::string  edge_type;
    int          formula_version    = 0;
    std::int64_t edge_version       = 0;
    // Stamped by EmbedEdgeWorker from live knowledge_edges.confidence.
    // Retrieval intents key the min_confidence Qdrant filter on this.
    // (Absent = 0.0 which fails any positive floor, so a candidate
    // written before this field was added is silently excluded.)
    double       confidence         = 0.0;
    std::string  endpoint_0_chunk_id;
    std::string  endpoint_1_chunk_id;
    std::string  review_state;
};

struct QdrantScoredEdge {
    std::string id;
    float       score = 0.0f;
    EdgePayload payload;
};

struct QdrantEdgeSearchResponse {
    std::vector<QdrantScoredEdge> result;
};

// Minimal JSON escape for user-facing text going into a Qdrant filter
// body. Point IDs and UUIDs are already validated shape.
std::string json_escape(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;
        }
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// QdrantRelationshipVectorStore
// ---------------------------------------------------------------------------

QdrantRelationshipVectorStore::QdrantRelationshipVectorStore(std::string qdrant_url,
                                                             std::string collection)
    : _qdrant_url(std::move(qdrant_url))
    , _collection(std::move(collection))
    , _client(drogon::HttpClient::newHttpClient(_qdrant_url))
{
    _client->setUserAgent("wikore-relationship-store/1.0");
}

drogon::Task<drogon::HttpResponsePtr>
QdrantRelationshipVectorStore::send(drogon::HttpMethod method,
                                    std::string_view path,
                                    std::string body,
                                    double timeout_s)
{
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(method);
    req->setPath(std::string(path));
    req->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    if (!body.empty()) req->setBody(std::move(body));
    co_return co_await _client->sendRequestCoro(req, timeout_s);
}

drogon::Task<Result<std::vector<EdgeCandidate>>>
QdrantRelationshipVectorStore::search(std::string_view                company_id,
                                      const Embedding&                query,
                                      const std::vector<std::string>& allowed_review_states,
                                      double                          min_confidence,
                                      int                             limit,
                                      double                          timeout_s)
{
    // Fail-closed: mirror the gate's short-circuit contract so a
    // caller-side bug (empty state whitelist) cannot leak candidates.
    if (allowed_review_states.empty() || limit <= 0)
        co_return std::vector<EdgeCandidate>{};

    std::string vec_json = "[";
    for (std::size_t i = 0; i < query.size(); ++i) {
        if (i) vec_json += ',';
        vec_json += std::format("{:.9g}", query[i]);
    }
    vec_json += ']';

    // Build the review_state MatchAny clause.
    std::string rs_json = "[";
    for (std::size_t i = 0; i < allowed_review_states.size(); ++i) {
        if (i) rs_json += ',';
        rs_json += std::format("\"{}\"", json_escape(allowed_review_states[i]));
    }
    rs_json += ']';

    // Filter: company_id equality, review_state ∈ whitelist, and (if
    // min_confidence > 0) confidence >= floor. The confidence range
    // filter uses Qdrant's Range primitive.
    std::string filter =
        std::format(R"({{"must":[)"
                    R"({{"key":"company_id","match":{{"value":"{}"}}}},)"
                    R"({{"key":"review_state","match":{{"any":{}}}}})",
                    json_escape(company_id), rs_json);
    if (min_confidence > 0.0) {
        filter += std::format(R"(,{{"key":"confidence","range":{{"gte":{:.6f}}}}})",
                              min_confidence);
    }
    filter += "]}";

    std::string body = std::format(
        R"({{"vector":{},"filter":{},"limit":{},"with_payload":true,"with_vector":false}})",
        vec_json, filter, limit);

    drogon::HttpResponsePtr resp;
    try {
        resp = co_await send(drogon::Post,
                             std::format("/collections/{}/points/search", _collection),
                             std::move(body), timeout_s);
    } catch (const std::exception& ex) {
        co_return std::unexpected(Error::unavailable(
            std::format("qdrant edge search: {}", ex.what())));
    }
    if (static_cast<int>(resp->getStatusCode()) != 200)
        co_return std::unexpected(Error::unavailable(
            std::format("qdrant edge search returned {}",
                        static_cast<int>(resp->getStatusCode()))));

    QdrantEdgeSearchResponse parsed;
    if (auto err = glz::read<glz::opts{.error_on_unknown_keys = false}>(
            parsed, resp->getBody()); err) {
        co_return std::unexpected(Error::unavailable(
            "qdrant edge search response parse failed"));
    }

    std::vector<EdgeCandidate> out;
    out.reserve(parsed.result.size());
    for (auto& r : parsed.result) {
        // Cross-tenant defense in depth: Qdrant already prefiltered on
        // company_id, but drop anything that doesn't match anyway. If
        // Qdrant ever returned a mismatch it would be a serious bug
        // upstream — log and drop rather than trust the response.
        if (r.payload.company_id != company_id) {
            spdlog::error("[relationship-store] Qdrant returned edge {} with "
                          "company_id='{}' but query was for '{}'; dropping",
                          r.id, r.payload.company_id, company_id);
            continue;
        }
        out.push_back(EdgeCandidate{
            .edge_id             = std::move(r.payload.edge_id),
            .company_id          = std::move(r.payload.company_id),
            .edge_type           = std::move(r.payload.edge_type),
            .score               = r.score,
            .edge_version        = r.payload.edge_version,
            .formula_version     = r.payload.formula_version,
            .endpoint_0_chunk_id = std::move(r.payload.endpoint_0_chunk_id),
            .endpoint_1_chunk_id = std::move(r.payload.endpoint_1_chunk_id),
        });
    }
    co_return out;
}

// ---------------------------------------------------------------------------
// NullRelationshipVectorStore
// ---------------------------------------------------------------------------

drogon::Task<Result<std::vector<EdgeCandidate>>>
NullRelationshipVectorStore::search(std::string_view                company_id,
                                    const Embedding&                /*query*/,
                                    const std::vector<std::string>& allowed_review_states,
                                    double                          min_confidence,
                                    int                             limit,
                                    double                          /*timeout_s*/)
{
    if (allowed_review_states.empty() || limit <= 0)
        co_return std::vector<EdgeCandidate>{};

    std::vector<EdgeCandidate> out;
    out.reserve(_entries.size());
    for (const auto& e : _entries) {
        if (e.candidate.company_id != company_id) continue;
        if (e.confidence < min_confidence)         continue;
        // review_state whitelist match
        bool ok = false;
        for (const auto& s : allowed_review_states)
            if (s == e.review_state) { ok = true; break; }
        if (!ok) continue;
        out.push_back(e.candidate);
        if (static_cast<int>(out.size()) >= limit) break;
    }
    co_return out;
}

void NullRelationshipVectorStore::add(EdgeCandidate ec, double confidence,
                                      std::string review_state)
{
    _entries.push_back(Entry{
        .candidate    = std::move(ec),
        .confidence   = confidence,
        .review_state = std::move(review_state),
    });
}

} // namespace wikore::rag
