#pragma once
#include "wikore/rag/types.hpp"
#include "wikore/domain/types.hpp"
#include "wikore/adapters/postgres/deadline_exec.hpp"
#include <drogon/drogon.h>
#include <string>
#include <string_view>
#include <vector>

namespace wikore::rag {

// ---------------------------------------------------------------------------
// EvidenceGate (Iteration 2) - the authoritative access boundary (G1).
//
// Converts retrieved ChunkCandidates into AllowedChunks by re-validating
// each against LIVE Postgres. Only EvidenceGate produces AllowedChunk (via
// AllowedChunk::ConstructionToken), so nothing reaches the reranker or
// ContextBuilder without passing through here.
// ---------------------------------------------------------------------------
class EvidenceGate {
public:
    explicit EvidenceGate(drogon::orm::DbClientPtr db) : db_(std::move(db)) {}

    drogon::Task<Result<std::vector<AllowedChunk>>>
    evaluate(std::string_view                   company_id,
             const AccessScope&                 scope,
             const std::vector<std::string>&    allowed_sensitivity_labels,
             const std::vector<ChunkCandidate>& candidates,
             const std::vector<std::string>&    lifecycle = {"active"},
             postgres::Deadline                 deadline = postgres::no_deadline()) const;

private:
    drogon::orm::DbClientPtr db_;
};

} // namespace wikore::rag
