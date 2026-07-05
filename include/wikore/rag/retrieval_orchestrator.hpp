#pragma once
#include "wikore/rag/types.hpp"
#include "wikore/rag/embedder.hpp"
#include "wikore/rag/vector_store.hpp"
#include "wikore/rag/evidence_gate.hpp"
#include "wikore/rag/relationship_evidence_gate.hpp"
#include "wikore/rag/relationship_vector_store.hpp"
#include "wikore/access_resolver.hpp"
#include "wikore/domain/types.hpp"
#include <drogon/drogon.h>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace wikore::rag {

// ---------------------------------------------------------------------------
// RetrievalIntent — the query orchestration surface.
//
//   Fact       — chunk-only retrieval. Same as the pre-BaryGraph read path;
//                unchanged behaviour and unchanged Iteration-2 contract.
//   Bridge     — edge-only retrieval. Runs the edge Qdrant search then the
//                RelationshipEvidenceGate (G2). No chunk candidates are
//                produced; the result stream contains only
//                AllowedRelationship values.
//   Automatic  — both candidate sources at once. Chunk and edge searches
//                run independently, feed independent gates (G1 and G2),
//                then the surviving results are merged and cut to `limit`.
//                Per docs §"Automatic intent": no classifier is required
//                for the first implementation. Score normalization across
//                kinds is intentionally naive (see the merge step's
//                comment) — a calibrated combined reranker is a
//                follow-up. Embed and access-scope resolution run exactly
//                once and are shared between the two sub-searches, so
//                Automatic costs one embed + two Qdrant searches + two
//                gates, not two of everything.
// ---------------------------------------------------------------------------
enum class RetrievalIntent { Fact, Bridge, Automatic };

// Bridge-intent tuning knobs. Kept in a struct so callers pass named
// arguments and future intents can add fields without breaking the
// signature.
struct BridgeIntentOptions {
    // Minimum edge confidence for a candidate to be returned by the
    // Qdrant search. The stored value comes from
    // knowledge_edges.confidence (0..1); 0.6 mirrors the design doc's
    // suggested bridge-intent floor.
    double min_confidence = 0.6;

    // Which review_state values Qdrant AND the gate will admit. Empty
    // is fail-closed (zero results).
    std::vector<std::string> allowed_review_states = {"accepted"};
};

// ---------------------------------------------------------------------------
// RetrievalOrchestrator (Iteration 3) - the read-path composition, now
// covering both fact and bridge intents.
//
// Behaviour unchanged for the existing fact-only entry point (retrieve()).
// The new retrieve_evidence() dispatches on intent and returns an
// AllowedEvidence variant list, ordered by candidate score within each
// source kind. Score normalization across chunk and edge is deferred to a
// combined reranker (step 8 territory); the orchestrator only guarantees
// that surviving candidates are handed off in score order per kind.
// ---------------------------------------------------------------------------
class RetrievalOrchestrator {
public:
    // over_fetch: search over_fetch * limit candidates before gating, so
    // gate drops do not starve the result set.
    //
    // The edge_store and edge_gate are OPTIONAL. Deployments that have not
    // yet stood up an edge Qdrant collection (or an admin who has not run
    // EmbedTypeVectorsUseCase) can construct the orchestrator without
    // them; bridge / automatic intents then return an actionable
    // ServiceUnavailable rather than silently degrading.
    RetrievalOrchestrator(
        std::shared_ptr<EmbedderPort>                    embedder,
        std::shared_ptr<AccessResolverPort>              resolver,
        std::shared_ptr<VectorStorePort>                 chunk_store,
        EvidenceGate                                     chunk_gate,
        std::shared_ptr<RelationshipVectorStorePort>     edge_store  = nullptr,
        std::shared_ptr<RelationshipEvidenceGate>        edge_gate   = nullptr,
        int                                              over_fetch  = 4)
        : embedder_(std::move(embedder))
        , resolver_(std::move(resolver))
        , chunk_store_(std::move(chunk_store))
        , chunk_gate_(std::move(chunk_gate))
        , edge_store_(std::move(edge_store))
        , edge_gate_(std::move(edge_gate))
        , over_fetch_(over_fetch) {}

    // Fact-only entry point (unchanged from the Iteration-2 contract).
    // scope_org_unit_id is the org_unit the query is scoped to (the
    // tenant root for a company-wide search).
    drogon::Task<Result<std::vector<AllowedChunk>>>
    retrieve(const RequestContext& ctx,
             std::string           query,
             std::string_view      scope_org_unit_id,
             int                   limit = 20) const;

    // Intent-dispatched entry point. Returns AllowedEvidence values in
    // the same order the underlying gates emitted them. Callers that
    // want a combined ranking should feed this through the reranker.
    drogon::Task<Result<std::vector<AllowedEvidence>>>
    retrieve_evidence(const RequestContext&      ctx,
                      std::string                query,
                      std::string_view           scope_org_unit_id,
                      RetrievalIntent            intent,
                      const BridgeIntentOptions& bridge_opts,
                      int                        limit = 20) const;

private:
    std::shared_ptr<EmbedderPort>                embedder_;
    std::shared_ptr<AccessResolverPort>          resolver_;
    std::shared_ptr<VectorStorePort>             chunk_store_;
    EvidenceGate                                 chunk_gate_;
    std::shared_ptr<RelationshipVectorStorePort> edge_store_;
    std::shared_ptr<RelationshipEvidenceGate>    edge_gate_;
    int                                          over_fetch_;
};

} // namespace wikore::rag
