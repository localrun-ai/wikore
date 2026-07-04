#pragma once
#include "wikore/rag/types.hpp"
#include "wikore/rag/embedder.hpp"
#include "wikore/rag/vector_store.hpp"
#include "wikore/rag/evidence_gate.hpp"
#include "wikore/access_resolver.hpp"
#include "wikore/domain/types.hpp"
#include <drogon/drogon.h>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace wikore::rag {

// ---------------------------------------------------------------------------
// RetrievalOrchestrator (Iteration 2) - the read-path composition.
//
//   query -> embed -> resolve reader scope -> derive clearance ->
//            build Qdrant prefilter -> vector search -> EvidenceGate ->
//            AllowedChunks.
//
// Access control is enforced twice - the Qdrant prefilter (recall fast path)
// and the EvidenceGate (authoritative boundary) - but both come from ONE
// resolved AccessScope and ONE clearance derivation, so the two layers can
// never disagree on policy. The prefilter may be stale (that only drops
// candidates); the gate resolves live and is the final say.
// ---------------------------------------------------------------------------
class RetrievalOrchestrator {
public:
    // over_fetch: search over_fetch * limit candidates before gating, so gate
    // drops do not starve the result set.
    RetrievalOrchestrator(std::shared_ptr<EmbedderPort>       embedder,
                          std::shared_ptr<AccessResolverPort> resolver,
                          std::shared_ptr<VectorStorePort>    vector_store,
                          EvidenceGate                        gate,
                          int                                 over_fetch = 4)
        : embedder_(std::move(embedder)),
          resolver_(std::move(resolver)),
          vector_store_(std::move(vector_store)),
          gate_(std::move(gate)),
          over_fetch_(over_fetch) {}

    // scope_org_unit_id is the org_unit the query is scoped to (the tenant
    // root for a company-wide search).
    drogon::Task<Result<std::vector<AllowedChunk>>>
    retrieve(const RequestContext& ctx,
             std::string           query,
             std::string_view      scope_org_unit_id,
             int                   limit = 20) const;

private:
    std::shared_ptr<EmbedderPort>       embedder_;
    std::shared_ptr<AccessResolverPort> resolver_;
    std::shared_ptr<VectorStorePort>    vector_store_;
    EvidenceGate                        gate_;
    int                                 over_fetch_;
};

} // namespace wikore::rag
