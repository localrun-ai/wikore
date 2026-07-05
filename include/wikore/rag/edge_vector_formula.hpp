#pragma once
#include "wikore/rag/types.hpp"
#include "wikore/domain/types.hpp"
#include <string_view>
#include <vector>

namespace wikore::rag {

// ---------------------------------------------------------------------------
// Edge-vector formula v1 (see docs/barygraph_features.md §"Wikore-adapted
// formula (formula_version = 1)" and the V037 migration header).
//
// edge_vector = normalize(
//     w_endpoint * (a_src * v_src + a_tgt * v_tgt)
//   + w_type    * v_type
// )
//
// where:
//   * a_src = max(auth_src, FLOOR) / (max(auth_src, FLOOR) + max(auth_tgt, FLOOR))
//   * a_tgt = 1 - a_src
//   * w_endpoint = 0.65, w_type = 0.35
//   * FLOOR = 10
//
// Pure function; no I/O. The 5b worker orchestrates the loads and calls
// this. Kept separate so the formula is unit-testable in isolation and
// so a future formula-v2 lives in its own file.
// ---------------------------------------------------------------------------

// Returns the normalized authority share for the source endpoint given
// two authority levels (0..100). Result is in [FLOOR/(FLOOR+100),
// 100/(FLOOR+100)]. Deterministic for equal inputs.
double compute_authority_share_v1(int authority_source, int authority_target);

// Compute the formula-v1 edge vector. Preconditions the caller MUST
// enforce (checked with assert in debug builds, error return in
// release):
//   * v_source.size() == v_target.size() == v_type.size() > 0
//   * authority values are in [0, 100]
// Returns std::unexpected(Error::invalid_state(...)) on precondition
// violation — programming bugs, not user input.
Result<Embedding> compute_edge_vector_v1(
    int              authority_source,
    int              authority_target,
    const Embedding& v_source,
    const Embedding& v_target,
    const Embedding& v_type);

} // namespace wikore::rag
