// Compile-time tests for AllowedEvidence type boundaries.
//
// These static_asserts verify that the ContextBuilder input type (AllowedEvidence)
// cannot be constructed from raw candidates that have not passed through EvidenceGate.
// They run at compile time with no runtime cost.

#include "wikore/rag/types.hpp"
#include "wikore/rag/context_builder.hpp"
#include <catch2/catch_test_macros.hpp>
#include <span>
#include <type_traits>

using namespace wikore::rag;
// ---------------------------------------------------------------------------
// Compile-time invariants
// ---------------------------------------------------------------------------

// ChunkCandidate is NOT convertible to AllowedEvidence — gate not bypassed.
static_assert(!std::is_convertible_v<ChunkCandidate, AllowedEvidence>,
    "ChunkCandidate must not be implicitly convertible to AllowedEvidence");

// AllowedChunk IS AllowedCandidate (alias, not a new type).
static_assert(std::is_same_v<AllowedChunk, AllowedCandidate>,
    "AllowedChunk must be exactly AllowedCandidate");

// AllowedEvidence holds exactly AllowedChunk (chunk-only, V1 scope).
static_assert(std::variant_size_v<AllowedEvidence> == 1,
    "AllowedEvidence must be a single-member variant (AllowedChunk only) until V034");

static_assert(std::is_same_v<std::variant_alternative_t<0, AllowedEvidence>, AllowedChunk>,
    "AllowedEvidence[0] must be AllowedChunk");

// ContextBuilder::build accepts span<const AllowedEvidence> — not raw candidates.
// Verified by: the function signature exists and compiles with span<AllowedEvidence>.
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

TEST_CASE("AllowedChunk is an alias for AllowedCandidate", "[allowed_evidence]")
{
    AllowedCandidate c;
    c.chunk_id           = "chunk-1";
    c.document_version_id = "ver-1";
    c.score              = 0.9f;
    c.text               = "Policy text.";

    // AllowedChunk and AllowedCandidate are the same type — assignment works.
    AllowedChunk ch = c;
    CHECK(ch.chunk_id == "chunk-1");
    CHECK(ch.text == "Policy text.");
}

TEST_CASE("AllowedEvidence wraps AllowedChunk", "[allowed_evidence]")
{
    AllowedChunk chunk{"chunk-2", "ver-2", 0.8f, "Some text.", std::nullopt};
    AllowedEvidence ev{chunk};

    CHECK(std::holds_alternative<AllowedChunk>(ev));
    CHECK(std::get<AllowedChunk>(ev).chunk_id == "chunk-2");
}

TEST_CASE("ContextBuilder::build produces non-empty prompt from chunk evidence",
          "[allowed_evidence][context_builder]")
{
    wikore::RequestContext ctx;
    ctx.tenant.company_id = "company-1";

    AllowedChunk c1{"c1", "v1", 0.9f, "Contractors require approval from Finance.", std::nullopt};
    AllowedChunk c2{"c2", "v2", 0.7f, "Finance controls contractor budget approval.", std::nullopt};

    std::vector<AllowedEvidence> evidence{c1, c2};

    ContextBuilderOptions opts;
    opts.system_prompt = "You are a helpful assistant.";

    ContextBuilder builder;
    auto result = builder.build(ctx, "Who approves contractor onboarding?",
                                evidence, opts);

    CHECK_FALSE(result.prompt.empty());
    CHECK(result.source_chunk_ids.size() == 2);
    CHECK(result.source_chunk_ids[0] == "c1");
    CHECK(result.source_chunk_ids[1] == "c2");
    CHECK(result.prompt.find("SRC 1") != std::string::npos);
    CHECK(result.prompt.find("Contractors require approval") != std::string::npos);
    CHECK(result.prompt.find("Who approves") != std::string::npos);
    CHECK(result.estimated_tokens > 0);
}

TEST_CASE("ContextBuilder::build respects max_evidence_items cap",
          "[allowed_evidence][context_builder]")
{
    wikore::RequestContext ctx;
    std::vector<AllowedEvidence> evidence;
    for (int i = 0; i < 10; ++i)
        evidence.push_back(AllowedChunk{
            std::to_string(i), "v", 0.5f, "text " + std::to_string(i), std::nullopt});

    ContextBuilderOptions opts;
    opts.max_evidence_items = 3;

    ContextBuilder builder;
    auto result = builder.build(ctx, "query", evidence, opts);

    // Only 3 items cited despite 10 provided.
    CHECK(result.source_chunk_ids.size() == 3);
}

TEST_CASE("ContextBuilder::build with empty evidence produces query-only prompt",
          "[allowed_evidence][context_builder]")
{
    wikore::RequestContext ctx;
    std::vector<AllowedEvidence> empty;

    ContextBuilder builder;
    auto result = builder.build(ctx, "What is the policy?", empty);

    CHECK(result.source_chunk_ids.empty());
    CHECK(result.prompt.find("What is the policy?") != std::string::npos);
}
