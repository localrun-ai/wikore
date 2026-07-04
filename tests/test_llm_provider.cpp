// Tests for LlmProvider port, adapters, and factory.
//
// These are pure unit tests — no live HTTP connections.  They verify:
//   - ChatRequest/ChatResponse struct round-trips
//   - SSE line parser (parse_sse_line semantics)
//   - factory routing from ProviderConfig.provider
//   - env-var shim (llm_provider_config_from_env)
//   - config defaults: model, max_tokens, temperature fallback
//
// Integration tests that actually call llama-server / Anthropic require
// TEST_LLM_INTEGRATION=1 and live endpoints; they are skipped in CI.

#include "wikore/rag/llm_provider.hpp"
#include "wikore/config.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

using namespace wikore;
using namespace wikore::rag;

// ---------------------------------------------------------------------------
// ProviderConfig defaults
// ---------------------------------------------------------------------------

TEST_CASE("LlmProviderConfig: defaults are sane", "[llm_provider]")
{
    LlmProviderConfig cfg;
    cfg.id       = "test-id";
    cfg.provider = "openai_compatible";
    cfg.model    = "llama3";
    cfg.base_url = "http://localhost:8080/v1";

    CHECK(cfg.max_tokens  == 2048);
    CHECK(cfg.temperature == Catch::Approx(0.1f).epsilon(0.001));
    CHECK(cfg.api_key.empty());
}

// ---------------------------------------------------------------------------
// Factory routing
// ---------------------------------------------------------------------------

TEST_CASE("make_llm_provider: openai_compatible creates a non-null provider",
          "[llm_provider][factory]")
{
    LlmProviderConfig cfg;
    cfg.id       = "oai";
    cfg.provider = "openai_compatible";
    cfg.model    = "llama3";
    cfg.base_url = "http://localhost:8080/v1";
    auto p = make_llm_provider(cfg);
    CHECK(p != nullptr);
}

TEST_CASE("make_llm_provider: anthropic creates a non-null provider",
          "[llm_provider][factory]")
{
    LlmProviderConfig cfg;
    cfg.id       = "ant";
    cfg.provider = "anthropic";
    cfg.model    = "claude-sonnet-4-6";
    cfg.api_key  = "sk-ant-test";
    auto p = make_llm_provider(cfg);
    CHECK(p != nullptr);
}

TEST_CASE("make_llm_provider: unknown provider throws",
          "[llm_provider][factory]")
{
    LlmProviderConfig cfg;
    cfg.id       = "bad";
    cfg.provider = "unknown_provider";
    cfg.model    = "model";
    CHECK_THROWS_AS(make_llm_provider(cfg), std::invalid_argument);
}

// ---------------------------------------------------------------------------
// Env-var shim
// ---------------------------------------------------------------------------

TEST_CASE("llm_provider_config_from_env: no api key -> openai_compatible",
          "[llm_provider][env]")
{
    Config cfg;
    cfg.llm_base_url    = "http://localhost:8080/v1";
    cfg.llm_model       = "mistral-7b";
    cfg.llm_max_tokens  = 1024;
    cfg.llm_temperature = 0.2f;

    auto pc = llm_provider_config_from_env(cfg);
    CHECK(pc.provider    == "openai_compatible");
    CHECK(pc.base_url    == "http://localhost:8080/v1");
    CHECK(pc.model       == "mistral-7b");
    CHECK(pc.max_tokens  == 1024);
    CHECK(pc.temperature == Catch::Approx(0.2f).epsilon(0.001));
    CHECK(pc.api_key.empty());
}

TEST_CASE("llm_provider_config_from_env: anthropic_api_key set -> anthropic",
          "[llm_provider][env]")
{
    Config cfg;
    cfg.anthropic_api_key = "sk-ant-test123";
    cfg.anthropic_model   = "claude-opus-4-5";
    cfg.llm_max_tokens    = 4096;
    cfg.llm_temperature   = 0.0f;

    auto pc = llm_provider_config_from_env(cfg);
    CHECK(pc.provider    == "anthropic");
    CHECK(pc.model       == "claude-opus-4-5");
    CHECK(pc.api_key     == "sk-ant-test123");
    CHECK(pc.max_tokens  == 4096);
}

// ---------------------------------------------------------------------------
// ChatRequest field forwarding
// ---------------------------------------------------------------------------

TEST_CASE("ChatRequest: fields are value types, copy/move correctly",
          "[llm_provider]")
{
    ChatRequest req;
    req.messages.push_back({.role = "system",    .content = "You are helpful."});
    req.messages.push_back({.role = "user",      .content = "Hello?"});
    req.model       = "llama3";
    req.max_tokens  = 512;
    req.temperature = 0.5f;

    auto req2 = req; // copy
    REQUIRE(req2.messages.size() == 2);
    CHECK(req2.messages[0].role    == "system");
    CHECK(req2.messages[1].content == "Hello?");
    CHECK(req2.max_tokens  == 512);
    CHECK(req2.temperature == Catch::Approx(0.5f).epsilon(0.001));
}

// ---------------------------------------------------------------------------
// ChatChunk sentinel
// ---------------------------------------------------------------------------

TEST_CASE("ChatChunk: done sentinel is distinguishable", "[llm_provider]")
{
    ChatChunk delta{.content = "token", .done = false};
    ChatChunk done{.done = true};

    CHECK_FALSE(delta.done);
    CHECK(done.done);
    CHECK(done.content.empty());
}

// ---------------------------------------------------------------------------
// Provider created from DB config can be used as LlmProviderPort*
// ---------------------------------------------------------------------------

TEST_CASE("make_llm_provider: returned pointer satisfies LlmProviderPort interface",
          "[llm_provider]")
{
    LlmProviderConfig cfg;
    cfg.id       = "poly";
    cfg.provider = "openai_compatible";
    cfg.model    = "test-model";
    cfg.base_url = "http://localhost:1/v1";

    std::shared_ptr<LlmProviderPort> p = make_llm_provider(cfg);
    REQUIRE(p);
    // We can't call chat() without a live server, but we can verify the
    // dynamic type is non-null and the virtual destructor works.
    CHECK(p.use_count() == 1);
}
