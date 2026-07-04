// ---------------------------------------------------------------------------
// LlmProvider factory + env-var compatibility shim.
// ---------------------------------------------------------------------------

#include "wikore/rag/llm_provider.hpp"
#include "wikore/config.hpp"
#include <spdlog/spdlog.h>
#include <stdexcept>

namespace wikore::rag {

// Forward declarations from the individual adapter translation units.
std::shared_ptr<LlmProviderPort> make_openai_compatible_provider(const LlmProviderConfig&);
std::shared_ptr<LlmProviderPort> make_azure_openai_provider(const LlmProviderConfig&);
std::shared_ptr<LlmProviderPort> make_anthropic_provider(const LlmProviderConfig&);

// ---------------------------------------------------------------------------
// GeminiStubAdapter — placeholder until the real adapter is implemented.
// Accepts construction so DB-configured gemini rows load without crashing;
// returns Error::unavailable on any call so callers get a clear message.
// ---------------------------------------------------------------------------
namespace {
class GeminiStubAdapter final : public LlmProviderPort {
public:
    explicit GeminiStubAdapter(LlmProviderConfig cfg) : cfg_(std::move(cfg)) {}

    drogon::Task<Result<ChatResponse>>
    chat(ChatRequest, double) const override {
        co_return std::unexpected(
            Error::unavailable("llm: gemini provider not yet implemented"));
    }
    drogon::Task<Result<ChatResponse>>
    chat_stream(ChatRequest, std::function<void(ChatChunk)>, double) const override {
        co_return std::unexpected(
            Error::unavailable("llm: gemini provider not yet implemented"));
    }
private:
    LlmProviderConfig cfg_;
};
} // namespace

std::shared_ptr<LlmProviderPort>
make_llm_provider(const LlmProviderConfig& cfg)
{
    if (cfg.provider == "openai_compatible")
        return make_openai_compatible_provider(cfg);
    if (cfg.provider == "azure_openai")
        return make_azure_openai_provider(cfg);
    if (cfg.provider == "anthropic")
        return make_anthropic_provider(cfg);
    if (cfg.provider == "gemini") {
        spdlog::info("[llm-factory] gemini provider configured but not yet implemented");
        return std::make_shared<GeminiStubAdapter>(cfg);
    }
    spdlog::error("[llm-factory] unsupported provider type '{}'", cfg.provider);
    throw std::invalid_argument("unsupported llm provider: " + cfg.provider);
}

LlmProviderConfig
llm_provider_config_from_env(const wikore::Config& cfg)
{
    // Prefer explicit Anthropic config when an API key is set.
    if (!cfg.anthropic_api_key.empty()) {
        return LlmProviderConfig{
            .id          = "env",
            .provider    = "anthropic",
            .display_name = "Anthropic (env)",
            .base_url    = {},
            .model       = cfg.anthropic_model,
            .api_key     = cfg.anthropic_api_key,
            .max_tokens  = cfg.llm_max_tokens,
            .temperature = cfg.llm_temperature,
        };
    }
    // Fall back to OpenAI-compatible (llama-server, Ollama, OpenAI, etc.)
    return LlmProviderConfig{
        .id           = "env",
        .provider     = "openai_compatible",
        .display_name = "LLM (env)",
        .base_url     = cfg.llm_base_url,
        .model        = cfg.llm_model,
        .api_key      = {},
        .max_tokens   = cfg.llm_max_tokens,
        .temperature  = cfg.llm_temperature,
    };
}

} // namespace wikore::rag
