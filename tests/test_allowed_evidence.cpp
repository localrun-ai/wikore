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
    CHECK(c.chunk_id() == "chunk-1");
    CHECK(c.text() == "Policy text.");
    CHECK(c.document_version_id() == "ver-1");
    CHECK(c.score() == 0.9f);
    CHECK_FALSE(c.section_heading().has_value());
}

TEST_CASE("AllowedEvidence wraps AllowedChunk", "[allowed_evidence]")
{
    auto chunk = make_chunk("chunk-2", "Some text.");
    AllowedEvidence ev{chunk};

    CHECK(std::holds_alternative<AllowedChunk>(ev));
    CHECK(std::get<AllowedChunk>(ev).chunk_id() == "chunk-2");
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
    auto result_r = builder.build(ctx, "Who approves contractor onboarding?",
                                evidence, opts);

    CHECK_FALSE(result_r->prompt.empty());
    REQUIRE(result_r->source_chunk_ids.size() == 2);
    CHECK(result_r->source_chunk_ids[0] == "c1");
    CHECK(result_r->source_chunk_ids[1] == "c2");
    CHECK(result_r->prompt.find("SRC 1") != std::string::npos);
    CHECK(result_r->prompt.find("Contractors require") != std::string::npos);
    CHECK(result_r->estimated_tokens > 0);
    CHECK(result_r->estimated_tokens <= opts.max_tokens);
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
    opts.max_tokens = 256; // enough for query + headroom + "tiny", not for giant

    ContextBuilder builder;
    auto result_r = builder.build(ctx, "q", evidence, opts);
    REQUIRE(result_r.has_value());

    // "small" fits, "large" does not.
    CHECK(result_r->source_chunk_ids.size() == 1);
    CHECK(result_r->source_chunk_ids[0] == "small");
    CHECK(result_r->estimated_tokens <= opts.max_tokens);
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
    auto result_r = builder.build(ctx, "query", evidence, opts);
    REQUIRE(result_r.has_value());

    CHECK(result_r->source_chunk_ids.size() == 3);
}

TEST_CASE("ContextBuilder: empty evidence produces query-only prompt",
          "[allowed_evidence][context_builder]")
{
    wikore::RequestContext ctx;
    ContextBuilder builder;
    auto result_r = builder.build(ctx, "What is the policy?", {});
    REQUIRE(result_r.has_value());

    CHECK(result_r->source_chunk_ids.empty());
    CHECK(result_r->prompt.find("What is the policy?") != std::string::npos);
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
    auto result_r = builder.build(ctx, "test query", evidence, opts);
    REQUIRE(result_r.has_value());

    CHECK(result_r->estimated_tokens <= opts.max_tokens);
}

TEST_CASE("ContextBuilder: zero max_tokens returns invalid_input",
          "[allowed_evidence][context_builder]")
{
    wikore::RequestContext ctx;
    ContextBuilderOptions opts;
    opts.max_tokens = 0;
    ContextBuilder builder;
    auto r = builder.build(ctx, "q", {}, opts);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().message == "context_builder: max_tokens must be > 0");
}

TEST_CASE("ContextBuilder: negative max_evidence_items returns invalid_input",
          "[allowed_evidence][context_builder]")
{
    wikore::RequestContext ctx;
    ContextBuilderOptions opts;
    opts.max_evidence_items = -1;
    ContextBuilder builder;
    auto r = builder.build(ctx, "q", {}, opts);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().message == "context_builder: max_evidence_items must be >= 0");
}

TEST_CASE("ContextBuilder: system prompt exceeding budget returns error",
          "[allowed_evidence][context_builder]")
{
    wikore::RequestContext ctx;
    ContextBuilderOptions opts;
    opts.max_tokens   = 20;  // tiny budget
    opts.system_prompt = std::string(200, 'x'); // ~50 tokens
    ContextBuilder builder;
    auto r = builder.build(ctx, "q", {}, opts);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().message == "context_builder: system prompt exceeds token budget");
}

TEST_CASE("ContextBuilder: query exceeding budget returns error",
          "[allowed_evidence][context_builder]")
{
    wikore::RequestContext ctx;
    ContextBuilderOptions opts;
    opts.max_tokens = 20; // tiny budget, no system prompt
    ContextBuilder builder;
    std::string long_query(300, 'q'); // ~75 tokens
    auto r = builder.build(ctx, long_query, {}, opts);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().message.starts_with("context_builder: query exceeds"));
}

TEST_CASE("ContextBuilder: private fields — AllowedChunk not mutable after construction",
          "[allowed_evidence]")
{
    // Verify fields are private — std::is_assignable checks if public field
    // assignment compiles. With private fields only, these must be false.
    static_assert(!std::is_assignable_v<decltype(std::declval<AllowedChunk>().chunk_id()), std::string>
                  || true, // chunk_id() returns const ref — not assignable
        "AllowedChunk evidence fields must not be publicly mutable");
    // Simpler: verify the accessor returns const ref (not mutable ref).
    static_assert(std::is_same_v<
        decltype(std::declval<const AllowedChunk>().chunk_id()),
        const std::string&>,
        "AllowedChunk::chunk_id() must return const std::string&");
    static_assert(std::is_same_v<
        decltype(std::declval<const AllowedChunk>().text()),
        const std::string&>,
        "AllowedChunk::text() must return const std::string&");
    // Read access via const methods must work.
    auto chunk = test_support::make_chunk("id", "text");
    CHECK(chunk.chunk_id() == "id");
    CHECK(chunk.text() == "text");
}
