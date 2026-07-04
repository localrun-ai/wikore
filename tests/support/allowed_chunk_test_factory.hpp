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

} // namespace wikore::rag::test_support
