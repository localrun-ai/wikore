#pragma once
#include "wikore/domain/types.hpp"
#include <openssl/evp.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace wikore::rag {

using Embedding = std::vector<float>;

// ---------------------------------------------------------------------------
// UUID v5 for deterministic Qdrant point IDs.
//
// Point ID = uuid_v5(url_namespace, chunk_id + ":" + embed_model_id)
// This means: same chunk + same model = same point ID across re-ingests,
// which makes upsert naturally idempotent.
//
// Returns std::nullopt on any OpenSSL failure (allocation or digest). The
// previous version returned the all-zero UUID sentinel, which silently
// collided every chunk in a batch onto a single point id -- all but one
// would vanish from Qdrant on upsert with no error surfaced. Returning
// nullopt forces the caller to handle the failure (typically by failing
// the outbox event so the next retry sees a healthy allocator).
// ---------------------------------------------------------------------------

inline std::optional<std::string> uuid_v5(std::string_view name) {
    // RFC 4122 URL namespace: 6ba7b811-9dad-11d1-80b4-00c04fd430c8
    static constexpr uint8_t kNs[16] = {
        0x6b, 0xa7, 0xb8, 0x11, 0x9d, 0xad, 0x11, 0xd1,
        0x80, 0xb4, 0x00, 0xc0, 0x4f, 0xd4, 0x30, 0xc8,
    };
    uint8_t      hash[20] = {0};
    unsigned int hash_len = sizeof(hash);

    // Use EVP API (OpenSSL 3.0+; SHA-1 low-level API is deprecated).
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx)
        return std::nullopt;

    bool ok =
           EVP_DigestInit_ex(ctx, EVP_sha1(), nullptr) == 1
        && EVP_DigestUpdate(ctx, kNs, 16)              == 1
        && EVP_DigestUpdate(ctx, name.data(), name.size()) == 1
        && EVP_DigestFinal_ex(ctx, hash, &hash_len)    == 1;
    EVP_MD_CTX_free(ctx);
    if (!ok)
        return std::nullopt;

    // Set version=5 and RFC 4122 variant bits.
    hash[6] = (hash[6] & 0x0f) | 0x50;
    hash[8] = (hash[8] & 0x3f) | 0x80;
    char buf[37];
    std::snprintf(buf, sizeof(buf),
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        hash[0], hash[1], hash[2], hash[3],
        hash[4], hash[5], hash[6], hash[7],
        hash[8], hash[9],
        hash[10], hash[11], hash[12], hash[13], hash[14], hash[15]);
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// ChunkPayload: every field stored as a Qdrant payload attribute.
//
// All fields are queryable as Qdrant filters without touching Postgres.
// Bump kSchemaVersion when adding/removing fields; the resync worker uses
// this to identify stale points that need payload refresh.
//
// V003 contract: the canonical names are document_id (struct) and
// owner_org_unit_id (the document's home OU). authority_level (0..100)
// comes from documents.authority_level and is consumed by the reranker
// to weight high-authority chunks (policy, mandate) above informational
// ones (notes, drafts). Both fields are written at ingest time and never
// mutated for an existing point id; if a document changes owner, ingest
// produces a new document_version and a new point id.
// ---------------------------------------------------------------------------

struct ChunkPayload {
    // v3 adds acl_version: the documents.acl_version stamped into the payload
    // at write time. It is a staleness HINT for the §2.1 pre-gate fast-path
    // (payload trustworthy iff acl_version >= documents.acl_version), never an
    // authorization authority -- G1 (EvidenceGate) resolves the resource axis
    // live regardless. The qdrant_resync_chunk_acl worker refreshes this field
    // (and access_scope_ids / sensitivity / lifecycle) without re-embedding
    // when an ACL change bumps documents.acl_version.
    static constexpr int kSchemaVersion = 3;

    std::string              company_id;
    std::string              document_id;
    std::string              document_version_id;
    // Optional because schema-v1 points written before this field existed
    // have no owner_org_unit_id key in their Qdrant payload; glaze deserializes
    // a missing string field as "" by default, and "" passed to a PostgreSQL
    // UUID parameter (e.g. the reranker's WHERE d.id = $1::uuid) raises
    // ERROR: invalid input syntax for type uuid. std::optional + glaze's
    // skip_null_members write path keeps round-trips faithful: v1 points
    // round-trip as nullopt, v2+ writes the real value. scheduler::ResyncWorker
    // refreshes this field for existing points on an ACL change; this keeps
    // pre-resync points safe in the meantime. Same rationale as section_id /
    // section_heading.
    std::optional<std::string> owner_org_unit_id;
    std::string              chunk_id;
    int                      chunk_index         = 0;
    int                      authority_level     = 50;   // documents.authority_level; default matches V003
    // All org_unit_ids whose members may retrieve this chunk:
    //   owner_org_unit_id + org_units with a resource_grant giving read access.
    // Used as the MatchAny filter in Qdrant queries.
    std::vector<std::string> access_scope_ids;
    std::string              sensitivity_label   = "internal";
    std::string              lifecycle_status    = "draft";
    // documents.acl_version at write time. Staleness hint only; see the
    // struct comment. Default 0 keeps schema-v1/v2 points (no acl_version key)
    // deserializing to the always-stale sentinel, so the pre-gate treats them
    // as untrusted and falls through to G1.
    std::int64_t             acl_version         = 0;
    std::optional<std::string> activated_at;     // ISO 8601
    std::optional<std::string> superseded_at;    // ISO 8601
    std::optional<std::string> section_id;
    std::optional<std::string> section_heading;
    int                      payload_schema_version = kSchemaVersion;
};

// ---------------------------------------------------------------------------
// ChunkCandidate: result of a Qdrant search before the EvidenceGate check.
// ---------------------------------------------------------------------------

struct ChunkCandidate {
    std::string  chunk_id;
    std::string  document_version_id;
    float        score   = 0.0f;
    ChunkPayload payload;
};

// ---------------------------------------------------------------------------
// EdgeCandidate: result of a Qdrant search on the edge collection, before the
// RelationshipEvidenceGate check.
//
// Payload fields come from V037's knowledge_edges_enqueue_upsert_fn writer
// (see EmbedEdgeWorker) — company_id, edge_id, edge_type, formula_version,
// edge_version, endpoint_0_chunk_id, endpoint_1_chunk_id, review_state.
// Everything is advisory: the gate MUST re-read the edge, its endpoints,
// and per-endpoint visibility from live Postgres. The candidate exists only
// to name a point for scoring and to feed the batch id list the gate
// materialises in one set-based query.
// ---------------------------------------------------------------------------

struct EdgeCandidate {
    std::string edge_id;
    std::string company_id;                   // advisory; re-verified by gate
    std::string edge_type;                    // advisory
    float       score              = 0.0f;    // Qdrant cosine similarity
    std::int64_t edge_version      = 0;       // advisory staleness hint
    int         formula_version    = 0;
    std::string endpoint_0_chunk_id;          // advisory; re-verified by gate
    std::string endpoint_1_chunk_id;          // advisory; re-verified by gate
};

#ifdef WIKORE_ENABLE_TEST_HOOKS
// Test-only construction hook. Release/production targets never see this
// declaration and therefore cannot define the friend to mint evidence.
namespace test_support { class TestGate; }
#endif

// ---------------------------------------------------------------------------
// AllowedChunk: a chunk that has passed EvidenceGate.
//
// Construction requires an AllowedChunk::ConstructionToken, which has a
// private constructor accessible only to EvidenceGate. This enforces at
// compile time that AllowedChunk can only be produced by the gate —
// synthesizing one without going through EvidenceGate requires an explicit
// knowledge of the token type and a friend relationship with EvidenceGate,
// which callers outside the rag:: module cannot obtain.
//
// Evidence fields are private and read-only: downstream code (Reranker,
// ContextBuilder) cannot replace text or IDs with ungated data after
// construction. The gate is the sole write path.
// ---------------------------------------------------------------------------

class AllowedChunk {
public:
    // PassKey — only EvidenceGate (production) and TestGate (tests) can
    // default-construct this token.
    class ConstructionToken {
        ConstructionToken() = default;
        friend class EvidenceGate;
#ifdef WIKORE_ENABLE_TEST_HOOKS
        friend class test_support::TestGate; // test targets only
#endif
    };

    AllowedChunk(ConstructionToken,
                 std::string  company_id,
                 std::string  chunk_id,
                 std::string  document_version_id,
                 float        score,
                 std::string  text,
                 std::optional<std::string> section_heading)
        : company_id_(std::move(company_id))
        , chunk_id_(std::move(chunk_id))
        , document_version_id_(std::move(document_version_id))
        , score_(score)
        , text_(std::move(text))
        , section_heading_(std::move(section_heading)) {}

    AllowedChunk() = delete;

    [[nodiscard]] const std::string& company_id()         const noexcept { return company_id_; }
    [[nodiscard]] const std::string& chunk_id()           const noexcept { return chunk_id_; }
    [[nodiscard]] const std::string& document_version_id()const noexcept { return document_version_id_; }
    [[nodiscard]] float              score()               const noexcept { return score_; }
    [[nodiscard]] const std::string& text()                const noexcept { return text_; }
    [[nodiscard]] const std::optional<std::string>& section_heading() const noexcept
                                                                      { return section_heading_; }

private:
    std::string  company_id_;
    std::string  chunk_id_;
    std::string  document_version_id_;
    float        score_   = 0.0f;
    std::string  text_;
    std::optional<std::string> section_heading_;
};

// ---------------------------------------------------------------------------
// AllowedRelationship: an edge that has passed RelationshipEvidenceGate.
//
// Same passkey construction pattern as AllowedChunk: only
// RelationshipEvidenceGate (and test_support::TestGate under the test-hooks
// macro) can mint one. Every endpoint is guaranteed to have been live-
// authorized at gate time; the gate hydrates the endpoint chunks' text
// alongside admission so the reranker and ContextBuilder never touch
// Qdrant payload text directly.
//
// Per docs/barygraph_features.md §"No partial relationship hydration": if
// ANY endpoint fails authorization the WHOLE relationship disappears — the
// gate never emits an AllowedRelationship for a partially-visible edge.
// Even the existence of the relationship is redacted from the caller,
// because revealing it can leak information about the inaccessible
// endpoint.
// ---------------------------------------------------------------------------

class RelationshipEvidenceGate;

class AllowedRelationship {
public:
    class ConstructionToken {
        ConstructionToken() = default;
        friend class RelationshipEvidenceGate;
#ifdef WIKORE_ENABLE_TEST_HOOKS
        friend class test_support::TestGate;
#endif
    };

    // Hydrated endpoint view. The gate carries the live chunk text +
    // section heading so downstream consumers (reranker, ContextBuilder)
    // never need to re-query Postgres or trust the Qdrant payload.
    struct AllowedEndpoint {
        int                        ordinal        = 0;  // 0 or 1
        std::string                chunk_id;
        std::string                document_version_id;
        std::string                text;
        std::optional<std::string> section_heading;
    };

    AllowedRelationship(ConstructionToken,
                        std::string  company_id,
                        std::string  edge_id,
                        std::string  edge_type,
                        std::string  direction,
                        std::string  origin,
                        float        score,
                        double       confidence,
                        std::string  review_state,
                        std::int64_t edge_version,
                        AllowedEndpoint ep0,
                        AllowedEndpoint ep1)
        : company_id_(std::move(company_id))
        , edge_id_(std::move(edge_id))
        , edge_type_(std::move(edge_type))
        , direction_(std::move(direction))
        , origin_(std::move(origin))
        , score_(score)
        , confidence_(confidence)
        , review_state_(std::move(review_state))
        , edge_version_(edge_version)
        , ep0_(std::move(ep0))
        , ep1_(std::move(ep1)) {}

    AllowedRelationship() = delete;

    [[nodiscard]] const std::string& company_id()   const noexcept { return company_id_; }
    [[nodiscard]] const std::string& edge_id()      const noexcept { return edge_id_; }
    [[nodiscard]] const std::string& edge_type()    const noexcept { return edge_type_; }
    [[nodiscard]] const std::string& direction()    const noexcept { return direction_; }
    // origin is a V034 knowledge_edges enum: 'administrator' (explicit
    // human decision), 'inference' (model-proposed), or 'deterministic'
    // (produced by a scheduled rule). Surfaced here because the
    // ContextBuilder rendering per docs §"Context construction" must
    // label inferred edges to prevent the LLM from claiming a strength
    // the edge does not have.
    [[nodiscard]] const std::string& origin()       const noexcept { return origin_; }
    [[nodiscard]] float              score()        const noexcept { return score_; }
    [[nodiscard]] double             confidence()   const noexcept { return confidence_; }
    [[nodiscard]] const std::string& review_state() const noexcept { return review_state_; }
    [[nodiscard]] std::int64_t       edge_version() const noexcept { return edge_version_; }
    [[nodiscard]] const AllowedEndpoint& endpoint_0() const noexcept { return ep0_; }
    [[nodiscard]] const AllowedEndpoint& endpoint_1() const noexcept { return ep1_; }

private:
    std::string     company_id_;
    std::string     edge_id_;
    std::string     edge_type_;
    std::string     direction_;
    std::string     origin_;
    float           score_        = 0.0f;
    double          confidence_   = 0.0;
    std::string     review_state_;
    std::int64_t    edge_version_ = 0;
    AllowedEndpoint ep0_;
    AllowedEndpoint ep1_;
};

// ---------------------------------------------------------------------------
// AllowedEvidence — the variant type accepted by ContextBuilder.
//
// Extended with AllowedRelationship once V034+ edges become retrievable.
// AllowedPath will follow when bounded-path traversal lands.
//
// ContextBuilder must accept std::span<const AllowedEvidence> and must NOT
// be overloaded for ChunkCandidate, raw Qdrant payloads, or diagnostic types.
// ---------------------------------------------------------------------------

using AllowedEvidence = std::variant<AllowedChunk, AllowedRelationship>;

// ---------------------------------------------------------------------------
// QdrantFilter: access-controlled search filter for a Qdrant query.
//
// QdrantFilterBuilder (Iteration 2) translates AccessScope + Principal
// into this struct. Used directly by VectorStorePort::search().
//
// sensitivity_labels is REQUIRED (no default). The retrieval-orchestrator is
// responsible for deriving the set of labels the requesting session may see
// (e.g. guest -> {public, internal}; member -> {public, internal,
// confidential}; clearance-bearing member -> {..., restricted}). Leaving it
// empty short-circuits the search to zero results -- the same fail-closed
// posture as an empty access_scope_ids. V014 is explicit that restricted
// content must never appear in generated answers, and the only way to enforce
// that at the index is to push the label set down as a filter; PG
// re-validation is defense-in-depth, not the primary gate.
// ---------------------------------------------------------------------------

struct QdrantFilter {
    std::string              company_id;
    std::vector<std::string> access_scope_ids;     // MatchAny on payload field
    std::vector<std::string> sensitivity_labels;   // MatchAny on payload field
    std::string              lifecycle_status = "active";
};

// ---------------------------------------------------------------------------
// PayloadPatch: the ACL-relevant subset of a chunk payload, refreshed by the
// qdrant_resync_chunk_acl worker without touching the vector. Applied via
// VectorStorePort::set_payload to a set of existing point ids. Every field is
// document-level (owner + grants) or version-level (sensitivity, lifecycle) and
// therefore identical across all chunks of one document version, so a single
// patch covers a whole document's point set.
// ---------------------------------------------------------------------------

struct PayloadPatch {
    std::vector<std::string>   access_scope_ids;
    std::string                sensitivity_label;
    std::string                lifecycle_status;
    std::optional<std::string> owner_org_unit_id;
    std::int64_t               acl_version            = 0;
    int                        payload_schema_version = ChunkPayload::kSchemaVersion;
};

// ---------------------------------------------------------------------------
// UpsertPoint: one point to write to the vector index.
// ---------------------------------------------------------------------------

struct UpsertPoint {
    std::string  id;       // uuid_v5(chunk_id + ":" + embed_model_id)
    Embedding    vector;
    ChunkPayload payload;
};

} // namespace wikore::rag
