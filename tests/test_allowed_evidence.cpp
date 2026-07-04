// Compile-time and runtime tests for AllowedEvidence type boundaries.

#include "wikore/rag/types.hpp"
#include "wikore/rag/context_builder.hpp"
#include "support/allowed_chunk_test_factory.hpp"
#include <catch2/catch_test_macros.hpp>
#include <span>
#include <type_traits>

using namespace wikore::rag;
using namespace wikore::rag::test_support;

// ---------------------------------------------------------------------------
// Compile-time invariants
// ---------------------------------------------------------------------------

// ChunkCandidate is NOT implicitly convertible to AllowedEvidence.
static_assert(!std::is_convertible_v<ChunkCandidate, AllowedEvidence>,
    "ChunkCandidate must not be implicitly convertible to AllowedEvidence");

// AllowedChunk is NOT default-constructible — token required.
static_assert(!std::is_default_constructible_v<AllowedChunk>,
    "AllowedChunk must not be default-constructible without ConstructionToken");

// AllowedChunk::ConstructionToken is NOT default-constructible by external code.
static_assert(!std::is_default_constructible_v<AllowedChunk::ConstructionToken>,
    "ConstructionToken must not be default-constructible by external callers");

// AllowedEvidence has exactly 1 member (chunk-only, V1 scope).
static_assert(std::variant_size_v<AllowedEvidence> == 1,
    "AllowedEvidence must be a single-member variant until V034");

static_assert(std::is_same_v<std::variant_alternative_t<0, AllowedEvidence>, AllowedChunk>,
    "AllowedEvidence[0] must be AllowedChunk");

// ContextBuilder::build is invocable with span<const AllowedEvidence>.
static_assert(
    std::is_invocable_v<
        decltype(&ContextBuilder::build),
        const ContextBuilder*,
        const wikore::RequestContext&,
        std::string_view,
        std::span<const AllowedEvidence>,
        const ContextBuilderOptions&>,
    "ContextBuilder::build must accept span<const AllowedEvidence>");

// ---------------------------------------------------------------------------
// Runtime tests
// ---------------------------------------------------------------------------

TEST_CASE("AllowedChunk: TestGate produces a valid chunk", "[allowed_evidence]")
{
    auto c = make_chunk("chunk-1", "Policy text.", "ver-1", 0.9f);
    CHECK(c.chunk_id == "chunk-1");
    CHECK(c.text == "Policy text.");
    CHECK(c.document_version_id == "ver-1");
    CHECK(c.score == 0.9f);
    CHECK_FALSE(c.section_heading.has_value());
}

TEST_CASE("AllowedEvidence wraps AllowedChunk", "[allowed_evidence]")
{
    auto chunk = make_chunk("chunk-2", "Some text.");
    AllowedEvidence ev{chunk};

    CHECK(std::holds_alternative<AllowedChunk>(ev));
    CHECK(std::get<AllowedChunk>(ev).chunk_id == "chunk-2");
}

TEST_CASE("ContextBuilder: produces non-empty prompt within token budget",
          "[allowed_evidence][context_builder]")
{
    wikore::RequestContext ctx;
    ctx.tenant.company_id = "company-1";

    std::vector<AllowedEvidence> evidence{
        make_chunk("c1", "Contractors require approval from Finance."),
        make_chunk("c2", "Finance controls contractor budget approval."),
    };

    ContextBuilderOptions opts;
    opts.system_prompt = "You are a helpful assistant.";
    opts.max_tokens    = 512;

    ContextBuilder builder;
    auto result = builder.build(ctx, "Who approves contractor onboarding?",
                                evidence, opts);

    CHECK_FALSE(result.prompt.empty());
    REQUIRE(result.source_chunk_ids.size() == 2);
    CHECK(result.source_chunk_ids[0] == "c1");
    CHECK(result.source_chunk_ids[1] == "c2");
    CHECK(result.prompt.find("SRC 1") != std::string::npos);
    CHECK(result.prompt.find("Contractors require") != std::string::npos);
    CHECK(result.estimated_tokens > 0);
    CHECK(result.estimated_tokens <= opts.max_tokens);
}

TEST_CASE("ContextBuilder: token budget excludes oversized chunks",
          "[allowed_evidence][context_builder]")
{
    wikore::RequestContext ctx;

    // One tiny chunk that fits, one giant chunk that won't.
    std::string giant(4096, 'x'); // ~1024 tokens on its own
    std::vector<AllowedEvidence> evidence{
        make_chunk("small", "tiny"),
        make_chunk("large", giant),
    };

    ContextBuilderOptions opts;
    opts.max_tokens = 64; // tight budget

    ContextBuilder builder;
    auto result = builder.build(ctx, "q", evidence, opts);

    // "small" fits, "large" does not.
    CHECK(result.source_chunk_ids.size() == 1);
    CHECK(result.source_chunk_ids[0] == "small");
    CHECK(result.estimated_tokens <= opts.max_tokens);
}

TEST_CASE("ContextBuilder: max_evidence_items cap",
          "[allowed_evidence][context_builder]")
{
    wikore::RequestContext ctx;
    std::vector<AllowedEvidence> evidence;
    for (int i = 0; i < 10; ++i)
        evidence.push_back(make_chunk(std::to_string(i), "text " + std::to_string(i)));

    ContextBuilderOptions opts;
    opts.max_evidence_items = 3;
    opts.max_tokens = 8192; // large budget, cap is item count only

    ContextBuilder builder;
    auto result = builder.build(ctx, "query", evidence, opts);

    CHECK(result.source_chunk_ids.size() == 3);
}

TEST_CASE("ContextBuilder: empty evidence produces query-only prompt",
          "[allowed_evidence][context_builder]")
{
    wikore::RequestContext ctx;
    ContextBuilder builder;
    auto result = builder.build(ctx, "What is the policy?", {});

    CHECK(result.source_chunk_ids.empty());
    CHECK(result.prompt.find("What is the policy?") != std::string::npos);
}

TEST_CASE("ContextBuilder: estimated_tokens never exceeds max_tokens",
          "[allowed_evidence][context_builder]")
{
    wikore::RequestContext ctx;
    std::vector<AllowedEvidence> evidence;
    for (int i = 0; i < 20; ++i)
        evidence.push_back(make_chunk(
            std::to_string(i),
            std::string(200, 'a'))); // ~50 tokens each

    ContextBuilderOptions opts;
    opts.max_tokens = 256;

    ContextBuilder builder;
    auto result = builder.build(ctx, "test query", evidence, opts);

    CHECK(result.estimated_tokens <= opts.max_tokens);
}
