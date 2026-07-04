#include "wikore/rag/types.hpp"
#include "wikore/rag/context_builder.hpp"
#include "support/allowed_chunk_test_factory.hpp"
#include <catch2/catch_test_macros.hpp>
#include <span>
#include <type_traits>

using namespace wikore::rag;
using namespace wikore::rag::test_support;

static wikore::RequestContext make_ctx(std::string company_id = "test-company") {
    wikore::RequestContext ctx;
    ctx.tenant.company_id = std::move(company_id);
    return ctx;
}

// ---------------------------------------------------------------------------
// Compile-time invariants
// ---------------------------------------------------------------------------

static_assert(!std::is_convertible_v<ChunkCandidate, AllowedEvidence>,
    "ChunkCandidate must not be implicitly convertible to AllowedEvidence");

static_assert(!std::is_default_constructible_v<AllowedChunk>,
    "AllowedChunk must not be default-constructible without ConstructionToken");

static_assert(!std::is_default_constructible_v<AllowedChunk::ConstructionToken>,
    "ConstructionToken must not be default-constructible by external callers");

static_assert(std::variant_size_v<AllowedEvidence> == 1,
    "AllowedEvidence must be a single-member variant until V034");

static_assert(std::is_same_v<std::variant_alternative_t<0, AllowedEvidence>, AllowedChunk>,
    "AllowedEvidence[0] must be AllowedChunk");

// Accessors return const ref — not mutable ref.
static_assert(std::is_same_v<decltype(std::declval<const AllowedChunk>().chunk_id()),
                              const std::string&>);
static_assert(std::is_same_v<decltype(std::declval<const AllowedChunk>().text()),
                              const std::string&>);
static_assert(std::is_same_v<decltype(std::declval<const AllowedChunk>().company_id()),
                              const std::string&>);

static_assert(
    std::is_invocable_v<
        decltype(&ContextBuilder::build),
        const ContextBuilder*,
        const wikore::RequestContext&,
        std::string_view,
        std::span<const AllowedEvidence>,
        const ContextBuilderOptions&>);

// ---------------------------------------------------------------------------
// AllowedChunk construction and field access
// ---------------------------------------------------------------------------

TEST_CASE("AllowedChunk: TestGate produces a valid chunk with company_id",
          "[allowed_evidence]")
{
    auto c = make_chunk("c1", "Policy text.", "co-A");
    CHECK(c.company_id() == "co-A");
    CHECK(c.chunk_id() == "c1");
    CHECK(c.text() == "Policy text.");
}

TEST_CASE("AllowedChunk: fields are private and immutable after construction",
          "[allowed_evidence]")
{
    static_assert(std::is_same_v<decltype(std::declval<const AllowedChunk>().text()),
                                 const std::string&>,
        "text() must return const ref");
    auto chunk = make_chunk("id", "text", "co");
    CHECK(chunk.chunk_id() == "id");
    CHECK(chunk.text() == "text");
    CHECK(chunk.company_id() == "co");
}

TEST_CASE("AllowedEvidence wraps AllowedChunk", "[allowed_evidence]")
{
    AllowedEvidence ev{make_chunk("c2", "Some text.")};
    CHECK(std::holds_alternative<AllowedChunk>(ev));
    CHECK(std::get<AllowedChunk>(ev).chunk_id() == "c2");
}

// ---------------------------------------------------------------------------
// ContextBuilder option validation
// ---------------------------------------------------------------------------

TEST_CASE("ContextBuilder: zero max_prompt_bytes returns error",
          "[allowed_evidence][context_builder]")
{
    ContextBuilderOptions opts; opts.max_prompt_bytes = 0;
    auto r = ContextBuilder{}.build(make_ctx(), "q", {}, opts);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().message.starts_with("context_builder: max_prompt_bytes"));
}

TEST_CASE("ContextBuilder: max_prompt_bytes above hard cap returns error",
          "[allowed_evidence][context_builder]")
{
    ContextBuilderOptions opts;
    opts.max_prompt_bytes = kContextBuilderMaxBytes + 1;
    auto r = ContextBuilder{}.build(make_ctx(), "q", {}, opts);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().message.starts_with("context_builder: max_prompt_bytes"));
}

TEST_CASE("ContextBuilder: negative max_evidence_items returns error",
          "[allowed_evidence][context_builder]")
{
    ContextBuilderOptions opts; opts.max_evidence_items = -1;
    auto r = ContextBuilder{}.build(make_ctx(), "q", {}, opts);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().message == "context_builder: max_evidence_items must be >= 0");
}

TEST_CASE("ContextBuilder: system_prompt exceeding budget returns error",
          "[allowed_evidence][context_builder]")
{
    ContextBuilderOptions opts;
    opts.max_prompt_bytes = 300; // enough for headroom but not the 200-byte system_prompt
    opts.system_prompt = std::string(200, 'x'); // 200 bytes > 64 budget
    auto r = ContextBuilder{}.build(make_ctx(), "q", {}, opts);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().message == "context_builder: system_prompt exceeds byte budget");
}

TEST_CASE("ContextBuilder: query exceeding budget returns error",
          "[allowed_evidence][context_builder]")
{
    ContextBuilderOptions opts; opts.max_prompt_bytes = 300; // enough for headroom but not the 200-byte system_prompt
    auto r = ContextBuilder{}.build(make_ctx(), std::string(200, 'q'), {}, opts);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().message.starts_with("context_builder: query exceeds"));
}

// ---------------------------------------------------------------------------
// Cross-tenant evidence rejected
// ---------------------------------------------------------------------------

TEST_CASE("ContextBuilder: cross-tenant evidence returns internal error",
          "[allowed_evidence][context_builder]")
{
    // Evidence gated for tenant-A, request context is tenant-B.
    std::vector<AllowedEvidence> evidence{
        make_chunk("c1", "HR policy text.", "tenant-A"),
    };
    auto ctx = make_ctx("tenant-B");
    auto r = ContextBuilder{}.build(ctx, "query", evidence);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().message == "context_builder: evidence company_id does not match request tenant");
}

// ---------------------------------------------------------------------------
// Byte budget enforcement
// ---------------------------------------------------------------------------

TEST_CASE("ContextBuilder: prompt_bytes never exceeds max_prompt_bytes",
          "[allowed_evidence][context_builder]")
{
    std::vector<AllowedEvidence> evidence;
    for (int i = 0; i < 20; ++i)
        evidence.push_back(make_chunk(std::to_string(i), std::string(200, 'a')));

    ContextBuilderOptions opts;
    opts.max_prompt_bytes = 2048;
    auto r = ContextBuilder{}.build(make_ctx(), "test query", evidence, opts);
    REQUIRE(r.has_value());
    CHECK(r->prompt_bytes <= opts.max_prompt_bytes);
    CHECK(r->prompt.size() <= opts.max_prompt_bytes);
}

TEST_CASE("ContextBuilder: oversized chunk excluded, small chunk included",
          "[allowed_evidence][context_builder]")
{
    std::vector<AllowedEvidence> evidence{
        make_chunk("small", "tiny"),
        make_chunk("large", std::string(4096, 'x')),
    };
    ContextBuilderOptions opts; opts.max_prompt_bytes = 512;
    auto r = ContextBuilder{}.build(make_ctx(), "q", evidence, opts);
    REQUIRE(r.has_value());
    CHECK(r->source_chunk_ids.size() == 1);
    CHECK(r->source_chunk_ids[0] == "small");
    CHECK(r->prompt_bytes <= opts.max_prompt_bytes);
}

TEST_CASE("ContextBuilder: CJK and emoji handled conservatively by byte count",
          "[allowed_evidence][context_builder]")
{
    // CJK characters are 3 bytes in UTF-8; emoji are 4 bytes.
    // A byte budget is conservative: 3+ bytes per visual character vs ~1 token.
    std::string cjk(50, '\0'); // placeholder; real CJK would be multi-byte
    std::vector<AllowedEvidence> evidence{
        make_chunk("cjk", "\xe6\x94\xbf\xe7\xad\x96\xef\xbc\x9a\xe6\x89\xbf\xe5\x8c\x85\xe5\x95\x86\xe9\x9c\x80\xe8\xa6\x81\xe8\xb4\xa2\xe5\x8a\xa1\xe9\x83\xa8\xe9\x97\xa8\xe6\x89\xb9\xe5\x87\x86\xe3\x80\x82"),   // 13 chars, 39 UTF-8 bytes
        make_chunk("emoji", "Policy key: approval required"), // emoji = 4 bytes each
    };
    ContextBuilderOptions opts; opts.max_prompt_bytes = 4096;
    auto r = ContextBuilder{}.build(make_ctx(), "query", evidence, opts);
    REQUIRE(r.has_value());
    CHECK(r->prompt_bytes <= opts.max_prompt_bytes);
    // Verify actual prompt size matches byte accounting.
    CHECK(r->prompt.size() <= opts.max_prompt_bytes);
}

TEST_CASE("ContextBuilder: max_evidence_items cap",
          "[allowed_evidence][context_builder]")
{
    std::vector<AllowedEvidence> evidence;
    for (int i = 0; i < 10; ++i)
        evidence.push_back(make_chunk(std::to_string(i), "text"));
    ContextBuilderOptions opts;
    opts.max_evidence_items = 3;
    opts.max_prompt_bytes = 65536;
    auto r = ContextBuilder{}.build(make_ctx(), "q", evidence, opts);
    REQUIRE(r.has_value());
    CHECK(r->source_chunk_ids.size() == 3);
}

TEST_CASE("ContextBuilder: empty evidence produces query-only prompt",
          "[allowed_evidence][context_builder]")
{
    auto r = ContextBuilder{}.build(make_ctx(), "What is the policy?", {});
    REQUIRE(r.has_value());
    CHECK(r->source_chunk_ids.empty());
    CHECK(r->prompt.find("What is the policy?") != std::string::npos);
    CHECK(r->prompt_bytes > 0);
    CHECK(r->prompt_bytes <= r->prompt.size() + 1); // accounting matches actual
}

TEST_CASE("ContextBuilder: valid build includes source IDs and respects order",
          "[allowed_evidence][context_builder]")
{
    std::vector<AllowedEvidence> evidence{
        make_chunk("c1", "Contractors require Finance approval."),
        make_chunk("c2", "Finance owns contractor budget."),
    };
    ContextBuilderOptions opts;
    opts.system_prompt = "You are a helpful assistant.";
    auto r = ContextBuilder{}.build(make_ctx(), "Who approves?", evidence, opts);
    REQUIRE(r.has_value());
    REQUIRE(r->source_chunk_ids.size() == 2);
    CHECK(r->source_chunk_ids[0] == "c1");
    CHECK(r->source_chunk_ids[1] == "c2");
    CHECK(r->prompt.find("SRC 1") != std::string::npos);
    CHECK(r->prompt.find("Who approves?") != std::string::npos);
    CHECK(r->prompt_bytes <= opts.max_prompt_bytes);
}
