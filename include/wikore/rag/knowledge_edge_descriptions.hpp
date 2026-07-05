#pragma once
#include "wikore/domain/knowledge_edge.hpp"
#include <array>
#include <string_view>
#include <utility>

namespace wikore::rag {

// ---------------------------------------------------------------------------
// kTypeDescriptions — stable product-owned prose that the admin embed job
// (application::EmbedTypeVectorsUseCase) turns into edge_type_vectors rows,
// keyed by (formula_version, edge_type, embedding_model_id).
//
// EVERY change to any string here MUST bump kFormulaVersion (below) and
// require a full re-index. Changing the text without bumping the formula
// version silently invalidates every existing Qdrant edge point without a
// migration path — the same point ID would then represent a semantically
// different vector.
//
// Source: docs/barygraph_features.md §"Relation-type vectors". Kept in
// sync with V037's edge_type_vectors table and with V034's
// knowledge_edges_edge_type_v1_chk vocabulary.
// ---------------------------------------------------------------------------

// Bumped when kTypeDescriptions or the edge-vector formula change.
// Formula-v2 will introduce w_source != w_target for directed types.
inline constexpr int kFormulaVersion = 1;

// Fixed weights for formula_version = 1. See the doc for rationale.
inline constexpr double kFormulaV1WeightEndpoint = 0.65;
inline constexpr double kFormulaV1WeightType     = 0.35;

// Authority floor: prevents a single authority=0 endpoint from
// contributing zero mass. Minimum endpoint share is FLOOR/(FLOOR+100)
// which at 10 is ~9%.
inline constexpr int kFormulaV1AuthorityFloor = 10;

inline constexpr std::array<std::pair<domain::EdgeType, std::string_view>, 9>
kTypeDescriptions{{
    {domain::EdgeType::implements,
     "one governed procedure operationalises and fulfils the other requirement"},
    {domain::EdgeType::depends_on,
     "this process cannot proceed without the other process or system"},
    {domain::EdgeType::exception_to,
     "this source states a bounded, scoped exception to the target rule"},
    {domain::EdgeType::contradicts,
     "these two governed statements impose incompatible requirements"},
    {domain::EdgeType::same_requirement_as,
     "these two statements express the same regulatory or policy requirement"},
    {domain::EdgeType::derived_from,
     "the source content is derived from or authored based on the target"},
    {domain::EdgeType::cites,
     "the source explicitly references and relies on the target as authority"},
    {domain::EdgeType::affects,
     "a change to the source directly affects the scope or validity of the target"},
    {domain::EdgeType::requires_approval_from,
     "the source action requires explicit authorisation from the target authority"},
}};

// Compile-time invariant: every V1 edge type has a description.
static_assert(kTypeDescriptions.size() == 9,
              "kTypeDescriptions must cover every V1 edge type in "
              "knowledge_edges_edge_type_v1_chk");

} // namespace wikore::rag
