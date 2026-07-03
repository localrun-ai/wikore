#pragma once
#include "wikore/rag/types.hpp"
#include "wikore/domain/types.hpp"
#include <drogon/HttpClient.h>
#include <drogon/utils/coroutine.h>
#include <string>
#include <string_view>
#include <vector>

namespace wikore::rag {

// ---------------------------------------------------------------------------
// VectorStorePort: abstract interface for the vector index.
//
// Implementations: QdrantVectorStore (production), NullVectorStore (tests).
// Owns both the write path (ingest) and read path (retrieval).
// ---------------------------------------------------------------------------

class VectorStorePort {
public:
    virtual ~VectorStorePort() = default;

    // Ensure the backing collection exists with the right vector size.
    // Idempotent: a 409 from Qdrant is treated as success.
    // Call once at startup before any upsert.
    virtual drogon::Task<Result<void>> ensure_collection(int dims) = 0;

    // Upsert a batch of points. Uses PUT /points which is idempotent:
    // re-ingesting the same chunk_id + model pair yields the same point ID
    // (via uuid_v5), so retries are safe.
    virtual drogon::Task<Result<void>>
    upsert(const std::vector<UpsertPoint>& points) = 0;

    // Delete all points belonging to a document version.
    // Called by the tombstone resync worker after lifecycle_status = 'archived'.
    virtual drogon::Task<Result<void>>
    delete_by_version(std::string_view company_id,
                      std::string_view document_version_id) = 0;

    // Overwrite the ACL-relevant payload keys on an existing set of points
    // WITHOUT re-embedding (Qdrant set-payload, a merge on the named keys).
    // Used by the qdrant_resync_chunk_acl worker: when a grant/owner/move
    // change bumps documents.acl_version, the chunk vectors are unchanged but
    // access_scope_ids / sensitivity / lifecycle / acl_version must be
    // refreshed. Empty point_ids is a no-op (success). Idempotent.
    virtual drogon::Task<Result<void>>
    set_payload(std::string_view                company_id,
                const std::vector<std::string>& point_ids,
                const PayloadPatch&             patch) = 0;

    // Search for the top-k most similar vectors that pass the access filter.
    // Returns ChunkCandidates in descending score order. timeout_s > 0 bounds
    // the call to that many seconds (callers pass the remaining request
    // deadline); 0 uses the adapter's default cap.
    virtual drogon::Task<Result<std::vector<ChunkCandidate>>>
    search(const Embedding& query,
           const QdrantFilter& filter,
           int limit = 20,
           double timeout_s = 0) = 0;
};

// ---------------------------------------------------------------------------
// QdrantVectorStore: adapter for Qdrant REST API.
//
// A single persistent HttpClientPtr is held per instance. Collection name
// defaults to "wikore_chunks_v1"; bump to v2 if the payload schema breaks.
// ---------------------------------------------------------------------------

class QdrantVectorStore : public VectorStorePort {
public:
    explicit QdrantVectorStore(std::string qdrant_url,
                               std::string collection = "wikore_chunks_v1");

    drogon::Task<Result<void>> ensure_collection(int dims) override;

    drogon::Task<Result<void>>
    upsert(const std::vector<UpsertPoint>& points) override;

    drogon::Task<Result<void>>
    delete_by_version(std::string_view company_id,
                      std::string_view document_version_id) override;

    drogon::Task<Result<void>>
    set_payload(std::string_view                company_id,
                const std::vector<std::string>& point_ids,
                const PayloadPatch&             patch) override;

    drogon::Task<Result<std::vector<ChunkCandidate>>>
    search(const Embedding& query,
           const QdrantFilter& filter,
           int limit = 20,
           double timeout_s = 0) override;

private:
    std::string           _qdrant_url;
    std::string           _collection;
    drogon::HttpClientPtr _client;

    // Helper: send JSON body to Qdrant, return response.
    // Throws on network error; callers check status code.
    drogon::Task<drogon::HttpResponsePtr>
    send(drogon::HttpMethod method,
         std::string_view path,
         std::string body = {},
         double timeout_s = 0);
};

// ---------------------------------------------------------------------------
// NullVectorStore: in-memory stub for tests (no Qdrant required).
//
// Upsert stores points in a vector. Search returns the stored points sorted
// by dot-product similarity (no real ANN, but deterministic). Delete clears
// matching entries. Sufficient for unit-testing the ingest use case.
// ---------------------------------------------------------------------------

class NullVectorStore : public VectorStorePort {
public:
    drogon::Task<Result<void>> ensure_collection(int /*dims*/) override {
        co_return Result<void>{};
    }

    drogon::Task<Result<void>>
    upsert(const std::vector<UpsertPoint>& points) override;

    drogon::Task<Result<void>>
    delete_by_version(std::string_view company_id,
                      std::string_view document_version_id) override;

    drogon::Task<Result<void>>
    set_payload(std::string_view                company_id,
                const std::vector<std::string>& point_ids,
                const PayloadPatch&             patch) override;

    drogon::Task<Result<std::vector<ChunkCandidate>>>
    search(const Embedding& query,
           const QdrantFilter& filter,
           int limit = 20,
           double timeout_s = 0) override;

    // Test introspection: total number of stored points.
    std::size_t point_count() const { return _points.size(); }

    // Test introspection: read back a stored point's payload by point id.
    const ChunkPayload* payload_for(std::string_view point_id) const {
        for (const auto& p : _points)
            if (p.id == point_id) return &p.payload;
        return nullptr;
    }

private:
    std::vector<UpsertPoint> _points;
};

} // namespace wikore::rag
