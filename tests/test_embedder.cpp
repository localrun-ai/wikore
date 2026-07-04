#include <catch2/catch_test_macros.hpp>
#include "wikore/rag/embedder.hpp"
#include "wikore/rag/types.hpp"
#include <cmath>

using namespace wikore::rag;

// NullEmbedder tests run synchronously using Catch2's coroutine helpers.
// We drive the coroutine to completion via a sync runner pattern:
// since NullEmbedder::embed suspends_never effectively (co_return of immediate
// value), we can get the result by manually stepping the coroutine.
//
// Catch2 v3 does not support co_await directly. We extract the value by
// running the Task on a Drogon sync_wait shim or by using .get() if available.
// Instead, we test the internal make_null_embedding helper via the public API
// by launching a minimal Drogon event loop iteration.
//
// For simplicity in unit tests (no event loop): test invariants that hold
// for ANY embedding rather than exact values, after confirming NullEmbedder
// produces results synchronously (initial_suspend = suspend_always in Drogon
// Tasks means we must drive the coroutine). We instead test the mathematical
// properties directly by inspecting the generated embedding through the
// Task's internal state via sync_wait.

// Drogon provides drogon::sync_wait for blocking awaits in non-coroutine contexts.
#include <drogon/utils/coroutine.h>

TEST_CASE("NullEmbedder: correct dimensionality", "[embedder]")
{
    NullEmbedder e4(4);
    NullEmbedder e8(8);

    CHECK(e4.dims() == 4);
    CHECK(e8.dims() == 8);
    CHECK(e4.model_name() == "null-embedder");
}

TEST_CASE("NullEmbedder: embed returns correct number of dimensions", "[embedder]")
{
    NullEmbedder emb(4);
    auto task   = emb.embed("hello");
    auto result = drogon::sync_wait(std::move(task));
    REQUIRE(result.has_value());
    CHECK(result->size() == 4);
}

TEST_CASE("NullEmbedder: embedding is unit-normalized", "[embedder]")
{
    NullEmbedder emb(16);
    auto result = drogon::sync_wait(emb.embed("normalize me"));
    REQUIRE(result.has_value());
    const auto& v = *result;
    float sum_sq = 0.0f;
    for (float x : v) sum_sq += x * x;
    CHECK(std::abs(std::sqrt(sum_sq) - 1.0f) < 1e-5f);
}

TEST_CASE("NullEmbedder: same input produces same embedding", "[embedder]")
{
    NullEmbedder emb(8);
    auto r1 = drogon::sync_wait(emb.embed("deterministic"));
    auto r2 = drogon::sync_wait(emb.embed("deterministic"));
    REQUIRE(r1.has_value());
    REQUIRE(r2.has_value());
    CHECK(*r1 == *r2);
}

TEST_CASE("NullEmbedder: different inputs produce different embeddings", "[embedder]")
{
    NullEmbedder emb(8);
    auto r1 = drogon::sync_wait(emb.embed("foo"));
    auto r2 = drogon::sync_wait(emb.embed("bar"));
    REQUIRE(r1.has_value());
    REQUIRE(r2.has_value());
    CHECK(*r1 != *r2);
}

TEST_CASE("NullEmbedder: embed_batch returns one embedding per input", "[embedder]")
{
    NullEmbedder emb(4);
    std::vector<std::string> texts = {"alpha", "beta", "gamma"};
    auto result = drogon::sync_wait(emb.embed_batch(texts));
    REQUIRE(result.has_value());
    REQUIRE(result->size() == 3);
    // Each must be 4-dimensional and unit-normalized
    for (const auto& v : *result) {
        REQUIRE(v.size() == 4);
        float ss = 0.0f;
        for (float x : v) ss += x * x;
        CHECK(std::abs(std::sqrt(ss) - 1.0f) < 1e-5f);
    }
    // All must differ
    CHECK((*result)[0] != (*result)[1]);
    CHECK((*result)[1] != (*result)[2]);
}

TEST_CASE("NullEmbedder: embed_batch result matches individual embed", "[embedder]")
{
    NullEmbedder emb(8);
    auto batch  = drogon::sync_wait(emb.embed_batch({"x", "y"}));
    auto single = drogon::sync_wait(emb.embed("x"));
    REQUIRE(batch.has_value());
    REQUIRE(single.has_value());
    CHECK((*batch)[0] == *single);
}

// ---------------------------------------------------------------------------
// EvidenceGate compile-time check:
// ChunkCandidate and AllowedChunk are distinct types (not aliases).
// ---------------------------------------------------------------------------
TEST_CASE("EvidenceGate: ChunkCandidate != AllowedChunk at compile time", "[types]")
{
    static_assert(!std::is_same_v<ChunkCandidate, AllowedChunk>,
                  "EvidenceGate: types must be distinct");
    // AllowedChunk has 'text' field; ChunkCandidate has 'payload'
    static_assert(requires(AllowedChunk a) { a.text(); }, "AllowedChunk must have text() accessor");
    static_assert(requires(ChunkCandidate   c) { c.payload; });
    SUCCEED("compile-time gate verified");
}
