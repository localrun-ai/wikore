#pragma once
#include "wikore/rag/types.hpp"
#include "wikore/domain/types.hpp"
#include <drogon/HttpClient.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/utils/coroutine.h>
#include <string>
#include <string_view>
#include <vector>

namespace wikore::rag {

// ---------------------------------------------------------------------------
// RelationshipVectorStorePort: read-side port for the Qdrant edge collection.
//
// Symmetric with VectorStorePort but scoped to the retrieval read path —
// the write side (upsert_raw / delete_points_by_id) already lives on
// VectorStorePort and is used by EmbedEdgeWorker / EdgeVectorCleanupWorker.
//
// This port exists so RetrievalOrchestrator (step 7) can search the edge
// collection with a tenant-scoped filter without needing to know the
// concrete adapter (Qdrant vs. Null-for-tests).
//
// Companion to RelationshipEvidenceGate: the gate consumes the port's
// EdgeCandidate output and, after live-authorization in Postgres,
// hydrates AllowedRelationship values.
// ---------------------------------------------------------------------------

class RelationshipVectorStorePort {
public:
    virtual ~RelationshipVectorStorePort() = default;

    // Search the edge collection for the top-k edges most similar to
    // `query`. `company_id` is a mandatory filter — edge points carry
    // company_id in their Qdrant payload (stamped by EmbedEdgeWorker),
    // so this is a Qdrant-side prefilter that never returns another
    // tenant's edges. It is NOT the authorization boundary: the
    // RelationshipEvidenceGate always re-verifies against live PG.
    //
    // `min_confidence` is a Qdrant-side floor on the edge's confidence
    // field (also in the payload); callers pass the retrieval intent's
    // configured floor (e.g. 0.6 for bridge intent). Zero disables the
    // filter.
    //
    // `allowed_review_states` restricts to review_state values the
    // caller trusts — typically {"accepted"} for user-visible retrieval,
    // {"accepted","proposed"} for diagnostics or preview intents.
    // Empty list is fail-closed (zero results), mirroring the chunk
    // gate's clearance contract.
    virtual drogon::Task<Result<std::vector<EdgeCandidate>>>
    search(std::string_view                company_id,
           const Embedding&                query,
           const std::vector<std::string>& allowed_review_states,
           double                          min_confidence,
           int                             limit,
           double                          timeout_s = 0) = 0;
};

// ---------------------------------------------------------------------------
// QdrantRelationshipVectorStore: adapter for the Qdrant edge collection.
//
// Reuses the underlying Qdrant HTTP transport but keeps the interface
// narrow — the port surface is read-only, and edge payloads have a
// different schema from chunk payloads (edge_id, edge_type,
// formula_version, ... instead of chunk_id, document_version_id, ...).
// ---------------------------------------------------------------------------

class QdrantRelationshipVectorStore : public RelationshipVectorStorePort {
public:
    QdrantRelationshipVectorStore(std::string qdrant_url,
                                  std::string collection);

    drogon::Task<Result<std::vector<EdgeCandidate>>>
    search(std::string_view                company_id,
           const Embedding&                query,
           const std::vector<std::string>& allowed_review_states,
           double                          min_confidence,
           int                             limit,
           double                          timeout_s = 0) override;

    // Response-parsing helper exposed for testing. Given the HTTP
    // status and body Qdrant returned, produces the same Result<>
    // that search() would after receiving that response. Split out
    // so a unit test can pin the 404-means-empty and the
    // non-200-means-unavailable semantics without needing a live
    // Qdrant.
    static Result<std::vector<EdgeCandidate>>
    parse_search_response(int              status_code,
                          std::string_view body,
                          std::string_view company_id,
                          std::string_view collection_name_for_log);

private:
    std::string           _qdrant_url;
    std::string           _collection;
    drogon::HttpClientPtr _client;

    drogon::Task<drogon::HttpResponsePtr>
    send(drogon::HttpMethod method,
         std::string_view path,
         std::string body,
         double timeout_s);
};

// ---------------------------------------------------------------------------
// NullRelationshipVectorStore: in-memory stub for tests. Stores edge points
// as (id, vector, payload_json) triples and returns them in stored order
// with a synthetic score. No cosine similarity — the gate side is what we
// actually want to exercise in tests.
// ---------------------------------------------------------------------------

class NullRelationshipVectorStore : public RelationshipVectorStorePort {
public:
    drogon::Task<Result<std::vector<EdgeCandidate>>>
    search(std::string_view                company_id,
           const Embedding&                query,
           const std::vector<std::string>& allowed_review_states,
           double                          min_confidence,
           int                             limit,
           double                          timeout_s = 0) override;

    // Test seed: add a candidate that will be returned by search() when
    // company_id + review_state + confidence all match the filter.
    // review_state is exposed so step 7's tests can seed 'proposed' or
    // other non-accepted entries alongside 'accepted' ones.
    void add(EdgeCandidate ec, double confidence, std::string review_state = "accepted");

private:
    struct Entry {
        EdgeCandidate candidate;
        double        confidence = 0.0;
        std::string   review_state;
    };
    std::vector<Entry> _entries;
};

} // namespace wikore::rag
