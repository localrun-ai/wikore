#pragma once
// ---------------------------------------------------------------------------
// AllowedChunk test factory — test targets only.
//
// Production binaries must never include this header. CMake enforces this:
// the tests/ directory is not in the include path for production targets.
//
// Why this exists: AllowedChunk::ConstructionToken has a private constructor
// accessible only to EvidenceGate. Unit tests that need AllowedChunk values
// without a live database use this factory instead of constructing via the
// gate directly.
//
// Usage:
//   #include "support/allowed_chunk_test_factory.hpp"
//   auto c = wikore::rag::test_support::make_chunk("id", "text");
// ---------------------------------------------------------------------------

#include "wikore/rag/types.hpp"
#include <optional>
#include <string>

namespace wikore::rag::test_support {

// TestToken: satisfies AllowedChunk(ConstructionToken, ...) by being a
// friend of ConstructionToken declared here. This is only reachable from
// test translation units.
class TestToken {
    // Befriend ConstructionToken so it can make one in tests.
    friend class AllowedChunk::ConstructionToken;

    // The token itself. We need to construct an AllowedChunk::ConstructionToken
    // but its constructor is private, gated to EvidenceGate.
    // Work around by making TestToken itself construct via its own private path:
    // declare TestToken as a friend of ConstructionToken by adding it there.
    // Since we can't modify the production header from here, we instead expose
    // a make() function that uses the test-only mechanism below.
};

// ---------------------------------------------------------------------------
// Production note: AllowedChunk::ConstructionToken must also declare
//   friend class wikore::rag::test_support::TestGate;
// in its definition (types.hpp) for this factory to work.
// The TestGate is only a friend inside test builds; production translation
// units never see this header and the friendship has no effect there.
// ---------------------------------------------------------------------------

class TestGate {
public:
    static AllowedChunk make(
        std::string company_id,
        std::string chunk_id,
        std::string text,
        std::string document_version_id = "test-ver",
        float       score               = 0.9f,
        std::optional<std::string> section_heading = std::nullopt)
    {
        return AllowedChunk{
            AllowedChunk::ConstructionToken{},
            std::move(company_id),
            std::move(chunk_id),
            std::move(document_version_id),
            score,
            std::move(text),
            std::move(section_heading),
        };
    }

    // Sibling factory for AllowedRelationship. Same passkey pattern —
    // AllowedRelationship::ConstructionToken befriends test_support::TestGate
    // under WIKORE_ENABLE_TEST_HOOKS, so this is only reachable from test
    // translation units.
    static AllowedRelationship make_rel(
        std::string company_id,
        std::string edge_id,
        std::string edge_type,
        AllowedRelationship::AllowedEndpoint ep0,
        AllowedRelationship::AllowedEndpoint ep1,
        std::string  direction    = "directed",
        std::string  origin       = "administrator",
        std::string  review_state = "accepted",
        float        score        = 0.9f,
        double       confidence   = 0.9,
        std::int64_t edge_version = 1)
    {
        return AllowedRelationship{
            AllowedRelationship::ConstructionToken{},
            std::move(company_id),
            std::move(edge_id),
            std::move(edge_type),
            std::move(direction),
            std::move(origin),
            score,
            confidence,
            std::move(review_state),
            edge_version,
            std::move(ep0),
            std::move(ep1),
        };
    }
};

// Convenience wrapper — defaults company_id to "test-company" for tests
// that don't care about tenant binding.
inline AllowedChunk make_chunk(
    std::string chunk_id,
    std::string text,
    std::string company_id          = "test-company",
    std::string document_version_id = "test-ver",
    float       score               = 0.9f,
    std::optional<std::string> section_heading = std::nullopt)
{
    return TestGate::make(std::move(company_id), std::move(chunk_id),
                          std::move(text), std::move(document_version_id),
                          score, std::move(section_heading));
}

// Convenience wrapper for AllowedRelationship. Fills reasonable defaults
// for the "administrator/accepted/directed" case; tests that need a
// different origin, review_state, or direction override them.
inline AllowedRelationship make_rel(
    std::string edge_id,
    std::string edge_type,
    AllowedRelationship::AllowedEndpoint ep0,
    AllowedRelationship::AllowedEndpoint ep1,
    std::string company_id   = "test-company",
    std::string direction    = "directed",
    std::string origin       = "administrator",
    std::string review_state = "accepted",
    float       score        = 0.9f,
    double      confidence   = 0.9,
    std::int64_t edge_version = 1)
{
    return TestGate::make_rel(
        std::move(company_id), std::move(edge_id), std::move(edge_type),
        std::move(ep0), std::move(ep1),
        std::move(direction), std::move(origin), std::move(review_state),
        score, confidence, edge_version);
}

// Convenience for constructing an endpoint inline.
inline AllowedRelationship::AllowedEndpoint make_endpoint(
    int ordinal, std::string chunk_id, std::string text,
    std::string role                = "source",
    std::string document_version_id = "test-ver",
    std::optional<std::string> section_heading = std::nullopt)
{
    return AllowedRelationship::AllowedEndpoint{
        .ordinal              = ordinal,
        .role                 = std::move(role),
        .chunk_id             = std::move(chunk_id),
        .document_version_id  = std::move(document_version_id),
        .text                 = std::move(text),
        .section_heading      = std::move(section_heading),
    };
}

} // namespace wikore::rag::test_support
