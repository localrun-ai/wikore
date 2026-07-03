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
// Converts retrieved ChunkCandidates into AllowedCandidates by re-validating
// each against LIVE Postgres in a single set-based query
// (docs/iteration_2_design.md section 0): tenant, lifecycle, sensitivity, and
// resource-axis visibility resolved from resource_grants + org_unit_closure
// (NOT the denormalized prefilter column, which lags). Candidates that fail to
// convert are dropped; survivors are hydrated (text, section_heading) from the
// same query and keep their retrieval score and order.
//
// "Qdrant is the index, Postgres is the evidence": the Qdrant prefilter can be
// stale, so it may only drop candidates here, never admit one the live state
// denies. Only the gate produces AllowedCandidate, so nothing reaches the
// reranker without passing through here.
//
// The section-5 item-1 property test (tests/test_retrieval_invariants.cpp)
// drives THIS gate directly against an independent oracle over randomized
// configs - there is no separate copy of the visibility SQL to drift from. It
// is what caught the principal_applies_to over-grant in the arms below.
// ---------------------------------------------------------------------------
class EvidenceGate {
public:
    explicit EvidenceGate(drogon::orm::DbClientPtr db) : db_(std::move(db)) {}

    // Returns only the candidates the authoritative state allows, hydrated and
    // in input (score) order. Fail-closed: empty scope, empty clearance, or no
    // candidates yields an empty result without a Postgres round-trip.
    // deadline bounds the DB round-trip to the remaining request budget
    // (Postgres statement_timeout); the default max() means unbounded.
    drogon::Task<Result<std::vector<AllowedCandidate>>>
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
