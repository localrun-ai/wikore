#include "wikore/rag/vector_store.hpp"
#include <glaze/glaze.hpp>
#include <drogon/HttpRequest.h>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <format>
#include <numeric>
#include <ranges>
#include <stdexcept>

namespace wikore::rag {

// Per-request timeout (seconds) for every Qdrant HTTP call. Without it,
// sendRequestCoro never returns if Qdrant stalls, hanging the caller
// indefinitely; on timeout the coro throws, which each send site converts to a
// Result error. Applied in the shared send() helper so all operations inherit
// it.
static constexpr double kQdrantTimeoutSecs = 30.0;

// ---------------------------------------------------------------------------
// JSON structs for Qdrant REST API
// ---------------------------------------------------------------------------

// PUT /collections/{name}
struct QdrantVectorsConfig {
    int    size;
    std::string distance = "Cosine";
};
struct QdrantCreateCollection {
    QdrantVectorsConfig vectors;
};

// PUT /collections/{name}/points (upsert)
struct QdrantPoint {
    std::string  id;
    Embedding    vector;
    ChunkPayload payload;
};
struct QdrantUpsertBody {
    std::vector<QdrantPoint> points;
};

// POST /collections/{name}/points/delete (delete by filter)
struct QdrantMatchValue {
    std::string value;
};
struct QdrantMatchAny {
    std::vector<std::string> any;
};
struct QdrantFieldCondition {
    std::string key;
    // Only one of these is populated per condition
    std::optional<QdrantMatchValue> match_value;
    std::optional<QdrantMatchAny>   match_any;
};

// We hand-build the delete + search JSON bodies as format strings to avoid
// deeply nested optional structs that add glaze complexity for no benefit.
// The bodies are small and the schema is stable.

// POST /collections/{name}/points/search
struct QdrantSearchResult {
    std::string  id;
    float        score = 0.0f;
    ChunkPayload payload;
};
struct QdrantSearchResponse {
    std::vector<QdrantSearchResult> result;
};

namespace {

// Minimal JSON string escaper for values that are interpolated into the
// hand-built search/delete bodies below. The inputs are validated UUIDs and
// a fixed enum today, so this is mostly defence-in-depth; if a future
// schema change broadens the field type, this prevents the JSON from
// breaking or becoming injectable.
std::string json_escape(std::string_view in)
{
    std::string out;
    out.reserve(in.size() + 2);
    for (char c : in) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b";  break;
        case '\f': out += "\\f";  break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += c;
            }
        }
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// QdrantVectorStore helpers
// ---------------------------------------------------------------------------

static std::string build_delete_body(std::string_view company_id,
                                     std::string_view version_id)
{
    return std::format(
        R"({{"filter":{{"must":[)"
        R"({{"key":"company_id","match":{{"value":"{}"}}}},)"
        R"({{"key":"document_version_id","match":{{"value":"{}"}}}})"
        R"(]}}}})",
        json_escape(company_id), json_escape(version_id));
}

static std::string build_search_body(const Embedding&    query,
                                     const QdrantFilter& filter,
                                     int                 limit)
{
    // Build vector array
    std::string vec_json = "[";
    for (size_t i = 0; i < query.size(); ++i) {
        if (i) vec_json += ',';
        vec_json += std::format("{:.8g}", query[i]);
    }
    vec_json += ']';

    auto build_string_array = [](const std::vector<std::string>& xs) {
        std::string out = "[";
        for (size_t i = 0; i < xs.size(); ++i) {
            if (i) out += ',';
            out += std::format(R"("{}")", json_escape(xs[i]));
        }
        out += ']';
        return out;
    };

    const std::string scope_json       = build_string_array(filter.access_scope_ids);
    const std::string sensitivity_json = build_string_array(filter.sensitivity_labels);

    return std::format(
        R"({{"vector":{},"limit":{},"with_payload":true,)"
        R"("filter":{{"must":[)"
        R"({{"key":"company_id","match":{{"value":"{}"}}}},)"
        R"({{"key":"access_scope_ids","match":{{"any":{}}}}},)"
        R"({{"key":"sensitivity_label","match":{{"any":{}}}}},)"
        R"({{"key":"lifecycle_status","match":{{"value":"{}"}}}})"
        R"(]}}}})",
        vec_json, limit,
        json_escape(filter.company_id), scope_json, sensitivity_json,
        json_escape(filter.lifecycle_status));
}

// ---------------------------------------------------------------------------
// QdrantVectorStore
// ---------------------------------------------------------------------------

QdrantVectorStore::QdrantVectorStore(std::string qdrant_url, std::string collection)
    : _qdrant_url(std::move(qdrant_url)), _collection(std::move(collection))
{
    _client = drogon::HttpClient::newHttpClient(_qdrant_url);
    _client->setPipeliningDepth(2);
}

drogon::Task<drogon::HttpResponsePtr>
QdrantVectorStore::send(drogon::HttpMethod method,
                        std::string_view   path,
                        std::string        body)
{
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(method);
    req->setPath(std::string(path));
    if (!body.empty()) {
        req->setContentTypeCode(drogon::CT_APPLICATION_JSON);
        req->setBody(std::move(body));
    }
    co_return co_await _client->sendRequestCoro(req, kQdrantTimeoutSecs);
}

drogon::Task<Result<void>>
QdrantVectorStore::ensure_collection(int dims)
{
    QdrantCreateCollection body{{dims, "Cosine"}};
    std::string json{};
    (void)glz::write_json(body, json);

    drogon::HttpResponsePtr resp;
    try {
        resp = co_await send(drogon::Put,
                             std::format("/collections/{}", _collection),
                             std::move(json));
    } catch (const std::exception& ex) {
        co_return std::unexpected(
            Error::unavailable(std::format("qdrant create_collection: {}", ex.what())));
    }

    auto code = static_cast<int>(resp->getStatusCode());
    // 200 = created, 409 = already exists - both are fine
    if (code != 200 && code != 409) {
        co_return std::unexpected(
            Error::unavailable(std::format("qdrant create_collection returned {}", code)));
    }
    co_return Result<void>{};
}

drogon::Task<Result<void>>
QdrantVectorStore::upsert(const std::vector<UpsertPoint>& points)
{
    if (points.empty())
        co_return Result<void>{};

    // Build JSON manually to avoid deep nesting issues with optional fields.
    // Each point: {"id":"...","vector":[...],"payload":{...}}
    std::string json = R"({"points":[)";
    for (size_t pi = 0; pi < points.size(); ++pi) {
        if (pi) json += ',';
        const auto& p = points[pi];

        // vector array
        std::string vec = "[";
        for (size_t i = 0; i < p.vector.size(); ++i) {
            if (i) vec += ',';
            vec += std::format("{:.8g}", p.vector[i]);
        }
        vec += ']';

        // payload - use glaze with skip_null option
        std::string payload_json{};
        (void)glz::write<glz::opts{.skip_null_members = true}>(p.payload, payload_json);

        json += std::format(R"({{"id":"{}","vector":{},"payload":{}}})",
                            p.id, vec, payload_json);
    }
    json += "]}";

    // wait=true: block until Qdrant has APPLIED the points, not merely accepted
    // them. The ingest worker takes the per-document resync advisory lock around
    // this write and refreshes ACL payload fields under it; committing/releasing
    // that lock before Qdrant applied the write would reopen the resync-vs-ingest
    // race the lock exists to close.
    drogon::HttpResponsePtr resp;
    try {
        resp = co_await send(drogon::Put,
                             std::format("/collections/{}/points?wait=true", _collection),
                             std::move(json));
    } catch (const std::exception& ex) {
        co_return std::unexpected(
            Error::unavailable(std::format("qdrant upsert: {}", ex.what())));
    }

    if (static_cast<int>(resp->getStatusCode()) != 200) {
        co_return std::unexpected(
            Error::unavailable(std::format("qdrant upsert returned {}",
                                           static_cast<int>(resp->getStatusCode()))));
    }
    co_return Result<void>{};
}

drogon::Task<Result<void>>
QdrantVectorStore::delete_by_version(std::string_view company_id,
                                     std::string_view document_version_id)
{
    auto body = build_delete_body(company_id, document_version_id);
    drogon::HttpResponsePtr resp;
    try {
        resp = co_await send(drogon::Post,
                             std::format("/collections/{}/points/delete", _collection),
                             std::move(body));
    } catch (const std::exception& ex) {
        co_return std::unexpected(
            Error::unavailable(std::format("qdrant delete_by_version: {}", ex.what())));
    }

    if (static_cast<int>(resp->getStatusCode()) != 200) {
        co_return std::unexpected(Error::unavailable(
            std::format("qdrant delete returned {}",
                        static_cast<int>(resp->getStatusCode()))));
    }
    co_return Result<void>{};
}

drogon::Task<Result<void>>
QdrantVectorStore::set_payload(std::string_view                company_id,
                               const std::vector<std::string>& point_ids,
                               const PayloadPatch&              patch)
{
    if (point_ids.empty())
        co_return Result<void>{};

    // Qdrant POST /points/payload sets (merges) the named keys on the listed
    // points, leaving the vector and any unlisted keys intact. We only rewrite
    // the ACL-relevant keys, so the embedding is never re-uploaded.
    std::string scope = "[";
    for (size_t i = 0; i < patch.access_scope_ids.size(); ++i) {
        if (i) scope += ',';
        scope += std::format("\"{}\"", patch.access_scope_ids[i]);
    }
    scope += ']';

    std::string owner = patch.owner_org_unit_id
        ? std::format("\"{}\"", *patch.owner_org_unit_id)
        : std::string("null");

    std::string ids = "[";
    for (size_t i = 0; i < point_ids.size(); ++i) {
        if (i) ids += ',';
        ids += std::format("\"{}\"", point_ids[i]);
    }
    ids += ']';

    // company_id is included so a misrouted patch cannot silently land on
    // another tenant's point: the id is a uuid_v5 of chunk+model and is not
    // tenant-scoped by itself, but the caller only ever collects ids from
    // its own tenant's rows. Writing company_id keeps the payload coherent.
    const std::string body = std::format(
        R"({{"payload":{{"company_id":"{}","access_scope_ids":{},"owner_org_unit_id":{},)"
        R"("sensitivity_label":"{}","lifecycle_status":"{}","acl_version":{},)"
        R"("payload_schema_version":{}}},"points":{}}})",
        company_id, scope, owner, patch.sensitivity_label, patch.lifecycle_status,
        patch.acl_version, patch.payload_schema_version, ids);

    // wait=true: block until Qdrant has APPLIED the payload, not merely accepted
    // it. The resync worker commits documents.qdrant_synced_version right after
    // this returns; without wait=true Qdrant's default acknowledges receipt
    // before the change is visible (or before it fails), so PG could advertise a
    // version as synced that Qdrant has not yet applied.
    drogon::HttpResponsePtr resp;
    try {
        resp = co_await send(drogon::Post,
                             std::format("/collections/{}/points/payload?wait=true", _collection),
                             body);
    } catch (const std::exception& ex) {
        co_return std::unexpected(
            Error::unavailable(std::format("qdrant set_payload: {}", ex.what())));
    }

    if (static_cast<int>(resp->getStatusCode()) != 200) {
        co_return std::unexpected(Error::unavailable(
            std::format("qdrant set_payload returned {}",
                        static_cast<int>(resp->getStatusCode()))));
    }
    co_return Result<void>{};
}

drogon::Task<Result<std::vector<ChunkCandidate>>>
QdrantVectorStore::search(const Embedding&    query,
                           const QdrantFilter& filter,
                           int                 limit)
{
    // Fail-closed before the network call. Qdrant currently treats an empty
    // MatchAny array as "match nothing", but relying on a remote system's
    // interpretation of an empty filter for a security-critical invariant
    // is fragile: a future server version or a misconfigured proxy that
    // strips the empty clause would silently widen visibility. Short-
    // circuiting here makes the contract local and explicit, and matches
    // NullVectorStore's behaviour so the two implementations agree byte-
    // for-byte from the caller's perspective.
    //
    // - empty access_scope_ids   -> caller has no resolved read scope.
    // - empty sensitivity_labels -> caller has no resolved clearance set.
    // Either case must return zero candidates. V014 requires that
    // restricted content never reach the LLM, which we cannot guarantee
    // if the filter that gates it can be silently dropped.
    if (filter.access_scope_ids.empty() || filter.sensitivity_labels.empty())
        co_return std::vector<ChunkCandidate>{};

    auto body = build_search_body(query, filter, limit);
    drogon::HttpResponsePtr resp;
    try {
        resp = co_await send(drogon::Post,
                             std::format("/collections/{}/points/search", _collection),
                             std::move(body));
    } catch (const std::exception& ex) {
        co_return std::unexpected(
            Error::unavailable(std::format("qdrant search: {}", ex.what())));
    }

    if (static_cast<int>(resp->getStatusCode()) != 200) {
        co_return std::unexpected(Error::unavailable(
            std::format("qdrant search returned {}",
                        static_cast<int>(resp->getStatusCode()))));
    }

    QdrantSearchResponse parsed{};
    if (auto err = glz::read_json(parsed, resp->getBody()); err) {
        co_return std::unexpected(Error::unavailable("qdrant search response parse failed"));
    }

    std::vector<ChunkCandidate> candidates;
    candidates.reserve(parsed.result.size());
    for (auto& r : parsed.result) {
        candidates.push_back({
            .chunk_id           = r.payload.chunk_id,
            .document_version_id = r.payload.document_version_id,
            .score              = r.score,
            .payload            = std::move(r.payload),
        });
    }
    co_return candidates;
}

// ---------------------------------------------------------------------------
// NullVectorStore
// ---------------------------------------------------------------------------

drogon::Task<Result<void>>
NullVectorStore::upsert(const std::vector<UpsertPoint>& points)
{
    for (const auto& p : points) {
        // Remove existing point with same ID (idempotent upsert)
        auto it = std::find_if(_points.begin(), _points.end(),
                               [&p](const UpsertPoint& x) { return x.id == p.id; });
        if (it != _points.end())
            *it = p;
        else
            _points.push_back(p);
    }
    co_return Result<void>{};
}

drogon::Task<Result<void>>
NullVectorStore::delete_by_version(std::string_view company_id,
                                    std::string_view document_version_id)
{
    std::erase_if(_points, [&](const UpsertPoint& p) {
        return p.payload.company_id         == company_id
            && p.payload.document_version_id == document_version_id;
    });
    co_return Result<void>{};
}

drogon::Task<Result<void>>
NullVectorStore::set_payload(std::string_view                company_id,
                             const std::vector<std::string>& point_ids,
                             const PayloadPatch&              patch)
{
    for (const auto& id : point_ids) {
        auto it = std::find_if(_points.begin(), _points.end(),
                               [&id](const UpsertPoint& p) { return p.id == id; });
        if (it == _points.end())
            continue;   // unknown point id: no-op, mirrors Qdrant merge semantics
        // Overwrite exactly the ACL-relevant keys; the vector and content-side
        // keys (chunk_id, section, etc.) are untouched.
        it->payload.company_id        = std::string(company_id);
        it->payload.access_scope_ids  = patch.access_scope_ids;
        it->payload.owner_org_unit_id = patch.owner_org_unit_id;
        it->payload.sensitivity_label = patch.sensitivity_label;
        it->payload.lifecycle_status  = patch.lifecycle_status;
        it->payload.acl_version       = patch.acl_version;
        it->payload.payload_schema_version = patch.payload_schema_version;
    }
    co_return Result<void>{};
}

drogon::Task<Result<std::vector<ChunkCandidate>>>
NullVectorStore::search(const Embedding&    query,
                         const QdrantFilter& filter,
                         int                 limit)
{
    // Match Qdrant payload-filter semantics:
    //   * an empty access_scope_ids MatchAny matches NOTHING (not "everything"),
    //     so an empty filter returns zero candidates. This is the same
    //     behaviour the production QdrantVectorStore produces, and it is the
    //     safe default: a caller with no scope must not see any evidence.
    //   * an empty sensitivity_labels MatchAny is treated the same way; the
    //     orchestrator is required to set the labels the session may see.
    //     V014: restricted content must never reach the LLM, and forgetting
    //     to populate the labels must not silently widen visibility.
    if (filter.access_scope_ids.empty() || filter.sensitivity_labels.empty())
        co_return std::vector<ChunkCandidate>{};

    std::vector<std::pair<float, const UpsertPoint*>> scored;
    scored.reserve(_points.size());

    for (const auto& p : _points) {
        if (p.payload.company_id       != filter.company_id)        continue;
        if (p.payload.lifecycle_status != filter.lifecycle_status)  continue;
        if (std::ranges::find(filter.sensitivity_labels,
                              p.payload.sensitivity_label)
            == filter.sensitivity_labels.end()) continue;
        bool in_scope = false;
        for (const auto& sid : filter.access_scope_ids) {
            if (std::ranges::find(p.payload.access_scope_ids, sid)
                != p.payload.access_scope_ids.end()) {
                in_scope = true;
                break;
            }
        }
        if (!in_scope) continue;

        // Dot product similarity (both vectors already unit-normalized for NullEmbedder)
        float dot = 0.0f;
        const size_t len = std::min(query.size(), p.vector.size());
        for (size_t i = 0; i < len; ++i)
            dot += query[i] * p.vector[i];
        scored.emplace_back(dot, &p);
    }

    std::sort(scored.begin(), scored.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });

    std::vector<ChunkCandidate> result;
    const int take = std::min(static_cast<int>(scored.size()), limit);
    result.reserve(take);
    for (int i = 0; i < take; ++i) {
        const auto& [score, pt] = scored[i];
        result.push_back({
            .chunk_id            = pt->payload.chunk_id,
            .document_version_id = pt->payload.document_version_id,
            .score               = score,
            .payload             = pt->payload,
        });
    }
    co_return result;
}

} // namespace wikore::rag
