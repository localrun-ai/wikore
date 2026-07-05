#include "wikore/rag/edge_vector_formula.hpp"
#include "wikore/rag/knowledge_edge_descriptions.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>

namespace wikore::rag {

double compute_authority_share_v1(int authority_source, int authority_target)
{
    // Clamp to [0, 100] defensively so a corrupted authority_level does
    // not produce a nonsensical share. In practice V003's CHECK enforces
    // this at the DB level, but the formula should still be total.
    const int s = std::clamp(authority_source, 0, 100);
    const int t = std::clamp(authority_target, 0, 100);
    const int s_floored = std::max(s, kFormulaV1AuthorityFloor);
    const int t_floored = std::max(t, kFormulaV1AuthorityFloor);
    const int denom     = s_floored + t_floored;
    // denom is at least 2 * FLOOR (= 20), so division is safe.
    return static_cast<double>(s_floored) / static_cast<double>(denom);
}

Result<Embedding> compute_edge_vector_v1(
    int              authority_source,
    int              authority_target,
    const Embedding& v_source,
    const Embedding& v_target,
    const Embedding& v_type)
{
    if (v_source.empty() || v_target.empty() || v_type.empty())
        return std::unexpected(Error::invalid_state(
            "edge-vector formula v1: all input vectors must be non-empty"));
    if (v_source.size() != v_target.size() || v_source.size() != v_type.size())
        return std::unexpected(Error::invalid_state(std::format(
            "edge-vector formula v1: dimension mismatch (source={}, target={}, type={})",
            v_source.size(), v_target.size(), v_type.size())));

    const double a_src = compute_authority_share_v1(authority_source, authority_target);
    const double a_tgt = 1.0 - a_src;

    Embedding out(v_source.size(), 0.0f);
    for (size_t i = 0; i < out.size(); ++i) {
        const double endpoint =
            a_src * static_cast<double>(v_source[i])
          + a_tgt * static_cast<double>(v_target[i]);
        out[i] = static_cast<float>(
            kFormulaV1WeightEndpoint * endpoint
          + kFormulaV1WeightType     * static_cast<double>(v_type[i]));
    }

    // L2-normalize. A zero-norm result (all inputs collinear to zero)
    // is a programming/data bug — the endpoint chunk vectors from an
    // embedding model should never all be zero — so we fail closed
    // rather than emit a degenerate point.
    double norm_sq = 0.0;
    for (float x : out) norm_sq += static_cast<double>(x) * static_cast<double>(x);
    const double norm = std::sqrt(norm_sq);
    if (!(norm > 0.0))
        return std::unexpected(Error::invalid_state(
            "edge-vector formula v1: zero-norm output — endpoint or type "
            "vector is degenerate"));
    for (float& x : out) x = static_cast<float>(static_cast<double>(x) / norm);
    return out;
}

} // namespace wikore::rag
