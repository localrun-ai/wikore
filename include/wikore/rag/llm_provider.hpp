#pragma once
#include "wikore/domain/types.hpp"
#include <drogon/drogon.h>
#include <functional>
#include <string>
#include <vector>

namespace wikore::rag {

// ---------------------------------------------------------------------------
// LLM provider port — Iteration 3.
//
// Abstracts over all supported LLM backends:
//   openai_compatible — llama.cpp, Ollama, vLLM, OpenAI, Groq, Together.ai,
//                       Mistral, Perplexity, Azure OpenAI.
//   anthropic         — Anthropic Messages API.
//   gemini            — Google Gemini REST API.
//
// Streaming model
// ---------------
// chat_stream() invokes on_chunk for every delta received from the upstream
// API's SSE stream and returns the accumulated ChatResponse (with token
// counts) when the stream completes.  The callback is invoked on the
// Drogon event-loop thread; callers must not block inside it.
//
// The chat controller (next PR) drives the SSE push to the browser by
// writing each ChatChunk into a Drogon chunked HTTP response from on_chunk.
//
// Thread safety: all implementations are stateless; multiple coroutines may
// call chat() / chat_stream() concurrently on the same instance.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Request / response types
// ---------------------------------------------------------------------------

struct ChatMessage {
    std::string role;     // "system" | "user" | "assistant"
    std::string content;
};

struct ChatRequest {
    std::vector<ChatMessage> messages;
    // If empty, the adapter uses the model from its provider config.
    std::string model;
    // 0 = use provider default (cfg.max_tokens).
    int         max_tokens  = 0;
    // Negative = use provider default (cfg.temperature).
    float       temperature = -1.0f;
};

// One streaming delta delivered via the on_chunk callback.
struct ChatChunk {
    std::string content;   // incremental text; empty on the final sentinel
    bool        done = false;
};

struct ChatResponse {
    std::string content;        // full generated text
    int         input_tokens  = 0;
    int         output_tokens = 0;
    std::string model;          // model name echoed by the provider
    std::string provider_id;    // llm_providers.id that served this request
};

// ---------------------------------------------------------------------------
// Port
// ---------------------------------------------------------------------------

class LlmProviderPort {
public:
    virtual ~LlmProviderPort() = default;

    // Non-streaming: waits for the full response.
    virtual drogon::Task<Result<ChatResponse>>
    chat(ChatRequest req, double timeout_s) const = 0;

    // Streaming: on_chunk is called for each incremental delta.
    // on_chunk(ChatChunk{.done=true}) is called once at the end of the stream.
    // Returns the accumulated ChatResponse (identical content to the last
    // non-done delta) when the stream completes.
    virtual drogon::Task<Result<ChatResponse>>
    chat_stream(ChatRequest                      req,
                std::function<void(ChatChunk)>   on_chunk,
                double                           timeout_s) const = 0;
};

// ---------------------------------------------------------------------------
// ProviderConfig: loaded from llm_providers DB row, passed to adapters.
// ---------------------------------------------------------------------------

struct LlmProviderConfig {
    std::string id;            // llm_providers.id (UUID)
    std::string provider;      // 'openai_compatible' | 'azure_openai' | 'anthropic' | 'gemini'
    std::string display_name;
    std::string base_url;      // required for openai_compatible and azure_openai
    std::string model;
    std::string api_key;       // decrypted; empty for local/unauthenticated
    std::string azure_api_version; // required for azure_openai (e.g. "2024-02-01")
    int         max_tokens  = 2048;
    float       temperature = 0.1f;
};

// ---------------------------------------------------------------------------
// Factory: creates the correct adapter for a given ProviderConfig.
// ---------------------------------------------------------------------------

std::shared_ptr<LlmProviderPort>
make_llm_provider(const LlmProviderConfig& cfg);

// Forward declaration (full definition in wikore/config.hpp).
} // namespace wikore::rag
namespace wikore { struct Config; }
namespace wikore::rag {

// Convenience: build a ProviderConfig from the legacy env-var Config fields
// (llm_base_url / llm_model) for backward compatibility with deployments that
// have not yet added a DB row.
LlmProviderConfig llm_provider_config_from_env(const wikore::Config& cfg);

} // namespace wikore::rag
