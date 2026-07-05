#pragma once
#include "wikore/rag/types.hpp"
#include "wikore/domain/types.hpp"
#include "wikore/adapters/postgres/deadline_exec.hpp"
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <string>
#include <string_view>
#include <vector>

namespace wikore::rag {

// ---------------------------------------------------------------------------
// RelationshipEvidenceGate — the G2 authorization boundary for edges.
//
// Symmetric with EvidenceGate (G1) for chunks. Converts EdgeCandidate rows
// (advisory Qdrant search results) into AllowedRelationship values by
// re-validating each edge and BOTH endpoints against LIVE Postgres:
//
//   * Edge exists in the caller's tenant.
//   * Edge state is admissible: review_state ∈ allowed_review_states,
//     valid lifecycle window (expires_at IS NULL OR > now), not
//     superseded.
//   * BOTH endpoints resolve to chunks whose owning documents pass the
//     same per-endpoint visibility check that G1 uses (owner-in-scope
//     OR live resource_grants). This is the whole-relationship
//     invariant from docs/barygraph_features.md §"No partial
//     relationship hydration": if EITHER endpoint fails visibility,
//     the WHOLE edge is redacted — the caller does not learn the edge
//     exists. Revealing existence can leak information about the
//     inaccessible endpoint.
//
// The gate hydrates both endpoint chunks' text + section heading in the
// same set-based query so downstream code never touches Qdrant payloads
// directly. Empty candidate list, empty scope, or empty clearance are
// fail-closed short-circuits (zero results), matching G1's contract.
//
// The SQL is a single set-based query — never one-authorization-per-
// endpoint. A batch of N edges produces one execSqlCoro call.
// ---------------------------------------------------------------------------

class RelationshipEvidenceGate {
public:
    explicit RelationshipEvidenceGate(drogon::orm::DbClientPtr db)
        : db_(std::move(db)) {}

    // No defaults on the security-gate signature. G1 makes callers pass
    // lifecycle and deadline explicitly for exactly the same reason: an
    // orchestrator that forgets the deadline gets an unbounded query on
    // the request path, and a lifecycle default silently hides
    // 'deprecated'/'archived' filtering decisions in the header.
    drogon::Task<Result<std::vector<AllowedRelationship>>>
    evaluate(std::string_view                    company_id,
             const AccessScope&                  scope,
             const std::vector<std::string>&     allowed_sensitivity_labels,
             const std::vector<std::string>&     allowed_review_states,
             const std::vector<EdgeCandidate>&   candidates,
             const std::vector<std::string>&     lifecycle,
             postgres::Deadline                  deadline) const;

private:
    drogon::orm::DbClientPtr db_;
};

} // namespace wikore::rag
