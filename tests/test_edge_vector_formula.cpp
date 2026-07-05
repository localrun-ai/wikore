#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "wikore/rag/edge_vector_formula.hpp"
#include "wikore/rag/knowledge_edge_descriptions.hpp"
#include <cmath>

using Catch::Matchers::WithinAbs;

// ---------------------------------------------------------------------------
// Pure unit tests for the edge-vector formula v1. No DB, no Qdrant.
// ---------------------------------------------------------------------------

TEST_CASE("compute_authority_share_v1: FLOOR clamps zero endpoints",
          "[edge_formula]")
{
    // Two zero-authority endpoints — both get clamped to FLOOR=10, so
    // the share splits 50/50. This is the zero-sum fallback the doc
    // describes without ever tripping a division by zero.
    CHECK(wikore::rag::compute_authority_share_v1(0, 0) == 0.5);

    // A single zero endpoint pulls the share toward the other, but is
    // still floored at FLOOR/(FLOOR + 100) = 10/110 ≈ 0.0909 — NOT zero.
    const double share_low  = wikore::rag::compute_authority_share_v1(0, 100);
    CHECK_THAT(share_low, WithinAbs(10.0 / 110.0, 1e-6));

    // Symmetric case: 100 vs 0 gives the source the complementary share.
    const double share_high = wikore::rag::compute_authority_share_v1(100, 0);
    CHECK_THAT(share_high, WithinAbs(100.0 / 110.0, 1e-6));
    CHECK_THAT(share_high + share_low, WithinAbs(1.0, 1e-6));
}

TEST_CASE("compute_authority_share_v1: equal authorities → 0.5",
          "[edge_formula]")
{
    CHECK_THAT(wikore::rag::compute_authority_share_v1(50, 50), WithinAbs(0.5, 1e-9));
    CHECK_THAT(wikore::rag::compute_authority_share_v1(75, 75), WithinAbs(0.5, 1e-9));
}

TEST_CASE("compute_authority_share_v1: policy weight beats procedure weight",
          "[edge_formula]")
{
    // The doc's motivating example: policy=90 implementing procedure=50.
    // Source share = 90 / (90 + 50) = 9/14 ≈ 0.643.
    const double share = wikore::rag::compute_authority_share_v1(90, 50);
    CHECK_THAT(share, WithinAbs(9.0 / 14.0, 1e-6));
    CHECK(share > 0.5);
}

TEST_CASE("compute_authority_share_v1: clamps out-of-range input defensively",
          "[edge_formula]")
{
    // V003's CHECK enforces [0, 100] at the DB level, but the formula
    // should still be total in case of corruption/schema drift.
    CHECK_THAT(wikore::rag::compute_authority_share_v1(-50, 50),
               WithinAbs(wikore::rag::compute_authority_share_v1(0, 50), 1e-9));
    CHECK_THAT(wikore::rag::compute_authority_share_v1(150, 50),
               WithinAbs(wikore::rag::compute_authority_share_v1(100, 50), 1e-9));
}

TEST_CASE("compute_edge_vector_v1: dimension mismatch is a programming bug",
          "[edge_formula]")
{
    wikore::rag::Embedding v_src{1.0f, 0.0f, 0.0f, 0.0f};
    wikore::rag::Embedding v_tgt{0.0f, 1.0f, 0.0f};       // wrong dim
    wikore::rag::Embedding v_type{0.0f, 0.0f, 1.0f, 0.0f};
    auto r = wikore::rag::compute_edge_vector_v1(50, 50, v_src, v_tgt, v_type);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().kind == wikore::Error::Kind::InvalidState);
}

TEST_CASE("compute_edge_vector_v1: zero-norm output is refused",
          "[edge_formula]")
{
    // All-zero inputs produce a zero-norm output; the formula must
    // fail closed rather than emit a degenerate point.
    wikore::rag::Embedding zero{0.0f, 0.0f, 0.0f, 0.0f};
    auto r = wikore::rag::compute_edge_vector_v1(50, 50, zero, zero, zero);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().kind == wikore::Error::Kind::InvalidState);
}

TEST_CASE("compute_edge_vector_v1: output is L2-normalized",
          "[edge_formula]")
{
    wikore::rag::Embedding v_src {1.0f, 0.0f, 0.0f, 0.0f};
    wikore::rag::Embedding v_tgt {0.0f, 1.0f, 0.0f, 0.0f};
    wikore::rag::Embedding v_type{0.0f, 0.0f, 1.0f, 0.0f};
    auto r = wikore::rag::compute_edge_vector_v1(50, 50, v_src, v_tgt, v_type);
    REQUIRE(r.has_value());
    double norm_sq = 0.0;
    for (float x : *r) norm_sq += static_cast<double>(x) * static_cast<double>(x);
    CHECK_THAT(std::sqrt(norm_sq), WithinAbs(1.0, 1e-5));
}

TEST_CASE("compute_edge_vector_v1: weights sum to 1 and split 0.65/0.35",
          "[edge_formula]")
{
    // Set up so we can check per-component contributions before
    // normalization. Equal authorities so a_src = a_tgt = 0.5.
    wikore::rag::Embedding v_src {2.0f, 0.0f, 0.0f, 0.0f};
    wikore::rag::Embedding v_tgt {0.0f, 2.0f, 0.0f, 0.0f};
    wikore::rag::Embedding v_type{0.0f, 0.0f, 4.0f, 0.0f};
    auto r = wikore::rag::compute_edge_vector_v1(50, 50, v_src, v_tgt, v_type);
    REQUIRE(r.has_value());

    // Pre-normalisation:
    //   out[0] = 0.65 * (0.5 * 2 + 0.5 * 0) + 0.35 * 0 = 0.65
    //   out[1] = 0.65 * (0.5 * 0 + 0.5 * 2) + 0.35 * 0 = 0.65
    //   out[2] = 0.65 * 0                   + 0.35 * 4 = 1.4
    // Norm = sqrt(0.65^2 + 0.65^2 + 1.4^2) = sqrt(0.845 + 1.96) = sqrt(2.805)
    const double norm = std::sqrt(0.65 * 0.65 * 2.0 + 1.4 * 1.4);
    CHECK_THAT((*r)[0], WithinAbs(0.65 / norm, 1e-5));
    CHECK_THAT((*r)[1], WithinAbs(0.65 / norm, 1e-5));
    CHECK_THAT((*r)[2], WithinAbs(1.4  / norm, 1e-5));
    CHECK_THAT((*r)[3], WithinAbs(0.0,          1e-6));
}

TEST_CASE("kTypeDescriptions covers every V1 edge type exactly once",
          "[edge_formula]")
{
    // Guards against a partial refactor of the enum or the descriptions
    // table by verifying every V1 EdgeType appears exactly once and
    // corresponds to a non-empty description.
    std::array<int, 9> seen{};
    for (const auto& [t, desc] : wikore::rag::kTypeDescriptions) {
        CHECK_FALSE(desc.empty());
        seen[static_cast<int>(t)] += 1;
    }
    for (int c : seen) CHECK(c == 1);
}
