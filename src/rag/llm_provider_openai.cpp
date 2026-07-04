// ---------------------------------------------------------------------------
// OpenAiCompatibleAdapter — POST /v1/chat/completions
//
// Covers all providers that speak the OpenAI chat-completions API:
//   Local:  llama.cpp server, Ollama, vLLM, LM Studio
//   Cloud:  OpenAI, Groq, Together.ai, Mistral, Perplexity, Azure OpenAI
//
// Streaming protocol (SSE):
//   The upstream sends "data: <json>\n\n" lines.
//   Each line carries choices[0].delta.content with a token fragment.
//   The stream ends with "data: [DONE]\n\n".
//   Usage (total token counts) arrives in the last data line or in a
//   separate x_groq / usage field depending on the provider; we read the
//   usage object if present.
//
// Non-streaming: same endpoint with stream=false; response is a single
// JSON object with choices[0].message.content.
// ---------------------------------------------------------------------------

#include "wikore/rag/llm_provider.hpp"
#include <drogon/HttpClient.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <glaze/glaze.hpp>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace wikore::rag {

namespace {

// ---------------------------------------------------------------------------
// JSON schemas (glaze)
// ---------------------------------------------------------------------------

struct OaiMessage {
    std::string role;
    std::string content;
};

struct OaiStreamOptions {
    bool include_usage = true;
};

struct OaiRequest {
    std::string              model;
    std::vector<OaiMessage>  messages;
    int                      max_tokens  = 2048;
    float                    temperature = 0.1f;
    bool                     stream      = false;
    // stream_options is only included when stream=true.
    // glaze will skip this field when stream=false because we set it only then.
    std::optional<OaiStreamOptions> stream_options;
};

// Non-streaming response
struct OaiChoice {
    OaiMessage message;
};
struct OaiUsage {
    int prompt_tokens     = 0;
    int completion_tokens = 0;
};
struct OaiResponse {
    std::vector<OaiChoice> choices;
    OaiUsage               usage;
    std::string            model;
};

// Streaming delta
struct OaiDelta {
    std::string content;
    std::string role;
};
struct OaiStreamChoice {
    OaiDelta    delta;
};
struct OaiStreamUsage {
    int prompt_tokens     = 0;
    int completion_tokens = 0;
};
struct OaiStreamChunk {
    std::vector<OaiStreamChoice> choices;
    OaiStreamUsage               usage;
    std::string                  model;
};

// ---------------------------------------------------------------------------
// SSE line parser: yields one JSON payload per "data: ..." line,
// skipping comment lines (": ...") and empty lines.
// Returns false when the "[DONE]" sentinel is encountered.
// ---------------------------------------------------------------------------
bool parse_sse_line(std::string_view line,
                    std::string&     json_out)
{
    if (line.starts_with("data:")) {
        auto payload = line.substr(5);
        // trim leading space
        if (!payload.empty() && payload[0] == ' ')
            payload.remove_prefix(1);
        if (payload == "[DONE]") return false;
        json_out = std::string(payload);
        return true;
    }
    // skip event:, id:, retry:, empty lines, comments
    return true; // continue, no payload
}

// ---------------------------------------------------------------------------
// OpenAiCompatibleAdapter
// ---------------------------------------------------------------------------

class OpenAiCompatibleAdapter final : public LlmProviderPort {
public:
    explicit OpenAiCompatibleAdapter(LlmProviderConfig cfg)
        : cfg_(std::move(cfg))
    {
        client_ = drogon::HttpClient::newHttpClient(cfg_.base_url);
        client_->setPipeliningDepth(0); // chat completions are long; no pipelining
    }

    drogon::Task<Result<ChatResponse>>
    chat(ChatRequest req, double timeout_s) const override
    {
        auto [body, err] = build_request(req, /*stream=*/false);
        if (!err.empty()) co_return std::unexpected(Error::invalid_input(err));

        auto http_req = drogon::HttpRequest::newHttpRequest();
        http_req->setMethod(drogon::Post);
        http_req->setPath("/v1/chat/completions");
        http_req->setContentTypeCode(drogon::CT_APPLICATION_JSON);
        http_req->setBody(std::move(body));
        set_auth_header(http_req);

        drogon::HttpResponsePtr resp;
        try {
            resp = co_await client_->sendRequestCoro(
                http_req, static_cast<float>(timeout_s));
        } catch (const std::exception& e) {
            spdlog::warn("[llm-openai] request failed: {}", e.what());
            co_return std::unexpected(Error::unavailable(
                std::format("llm: upstream error: {}", e.what())));
        }

        if (resp->getStatusCode() != drogon::k200OK) {
            spdlog::warn("[llm-openai] HTTP {} from {}: {}",
                static_cast<int>(resp->getStatusCode()),
                cfg_.base_url, resp->getBody());
            co_return std::unexpected(Error::unavailable(
                std::format("llm: HTTP {}", static_cast<int>(resp->getStatusCode()))));
        }

        OaiResponse oai_resp;
        auto parse_err = glz::read_json(oai_resp, resp->getBody());
        if (parse_err) {
            spdlog::warn("[llm-openai] JSON parse error: {}",
                         glz::format_error(parse_err, resp->getBody()));
            co_return std::unexpected(Error::unavailable("llm: invalid response JSON"));
        }
        if (oai_resp.choices.empty()) {
            co_return std::unexpected(Error::unavailable("llm: empty choices in response"));
        }

        co_return ChatResponse{
            .content       = oai_resp.choices[0].message.content,
            .input_tokens  = oai_resp.usage.prompt_tokens,
            .output_tokens = oai_resp.usage.completion_tokens,
            .model         = oai_resp.model.empty() ? cfg_.model : oai_resp.model,
            .provider_id   = cfg_.id,
        };
    }

    drogon::Task<Result<ChatResponse>>
    chat_stream(ChatRequest                    req,
                std::function<void(ChatChunk)> on_chunk,
                double                         timeout_s) const override
    {
        auto [body, err] = build_request(req, /*stream=*/true);
        if (!err.empty()) co_return std::unexpected(Error::invalid_input(err));

        auto http_req = drogon::HttpRequest::newHttpRequest();
        http_req->setMethod(drogon::Post);
        http_req->setPath("/v1/chat/completions");
        http_req->setContentTypeCode(drogon::CT_APPLICATION_JSON);
        http_req->setBody(std::move(body));
        set_auth_header(http_req);

        // sendRequestCoro buffers the full SSE body then returns; we parse the
        // accumulated SSE lines and invoke on_chunk synchronously here.
        // This means the browser SSE push starts only after the LLM has finished
        // generating — acceptable for the initial implementation.  A future PR can
        // switch to Drogon's streaming response API (newStreamHttpClient) to reduce
        // TTFT.  The on_chunk callback contract and ChatResponse return type are
        // stable across both approaches.
        drogon::HttpResponsePtr resp;
        try {
            resp = co_await client_->sendRequestCoro(
                http_req, static_cast<float>(timeout_s));
        } catch (const std::exception& e) {
            spdlog::warn("[llm-openai] stream request failed: {}", e.what());
            co_return std::unexpected(Error::unavailable(
                std::format("llm: upstream error: {}", e.what())));
        }

        if (resp->getStatusCode() != drogon::k200OK) {
            spdlog::warn("[llm-openai] stream HTTP {}", static_cast<int>(resp->getStatusCode()));
            co_return std::unexpected(Error::unavailable(
                std::format("llm: HTTP {}", static_cast<int>(resp->getStatusCode()))));
        }

        std::string accumulated;
        int input_tokens  = 0;
        int output_tokens = 0;
        std::string model_echo;

        // Parse SSE body: split on '\n', process each line.
        std::string_view raw = resp->getBody();
        std::size_t pos = 0;
        while (pos < raw.size()) {
            auto nl = raw.find('\n', pos);
            std::string_view line = (nl == std::string_view::npos)
                ? raw.substr(pos)
                : raw.substr(pos, nl - pos);
            // strip trailing \r for CRLF streams
            if (!line.empty() && line.back() == '\r')
                line.remove_suffix(1);
            pos = (nl == std::string_view::npos) ? raw.size() : nl + 1;

            if (line.empty()) continue;

            std::string json_payload;
            if (!parse_sse_line(line, json_payload)) break; // [DONE]
            if (json_payload.empty()) continue;

            OaiStreamChunk chunk;
            if (glz::read_json(chunk, json_payload)) continue; // skip unparseable

            if (!chunk.choices.empty()) {
                const auto& delta = chunk.choices[0].delta;
                if (!delta.content.empty()) {
                    accumulated += delta.content;
                    on_chunk(ChatChunk{.content = delta.content, .done = false});
                }
            }
            // Capture usage if provided (some providers send it on last chunk)
            if (chunk.usage.prompt_tokens > 0)
                input_tokens = chunk.usage.prompt_tokens;
            if (chunk.usage.completion_tokens > 0)
                output_tokens = chunk.usage.completion_tokens;
            if (!chunk.model.empty() && model_echo.empty())
                model_echo = chunk.model;
        }

        on_chunk(ChatChunk{.done = true});

        co_return ChatResponse{
            .content       = std::move(accumulated),
            .input_tokens  = input_tokens,
            .output_tokens = output_tokens,
            .model         = model_echo.empty() ? cfg_.model : model_echo,
            .provider_id   = cfg_.id,
        };
    }

private:
    LlmProviderConfig                       cfg_;
    mutable std::shared_ptr<drogon::HttpClient> client_;

    std::pair<std::string, std::string> // (body, error)
    build_request(const ChatRequest& req, bool stream) const
    {
        OaiRequest oai;
        oai.model       = req.model.empty() ? cfg_.model : req.model;
        // 0 = sentinel: use provider default
        oai.max_tokens  = (req.max_tokens > 0) ? req.max_tokens : cfg_.max_tokens;
        // negative = sentinel: use provider default
        oai.temperature = (req.temperature >= 0) ? req.temperature : cfg_.temperature;
        oai.stream      = stream;
        // Ask providers to include token usage in the final streaming chunk.
        // OpenAI requires stream_options.include_usage=true for this; omitting
        // it leaves usage at zero.
        if (stream)
            oai.stream_options = OaiStreamOptions{.include_usage = true};
        oai.messages.reserve(req.messages.size());
        for (const auto& m : req.messages)
            oai.messages.push_back({.role = m.role, .content = m.content});
        if (oai.model.empty())
            return {"", "llm: no model configured"};

        std::string body;
        auto err = glz::write_json(oai, body);
        if (err) return {"", "llm: failed to serialise request"};
        return {std::move(body), ""};
    }

    void set_auth_header(drogon::HttpRequestPtr& req) const
    {
        if (!cfg_.api_key.empty())
            req->addHeader("Authorization", "Bearer " + cfg_.api_key);
    }
};

// ---------------------------------------------------------------------------
// AzureOpenAiAdapter — Azure OpenAI deployment endpoint.
//
// Azure OpenAI has a different URL scheme and auth header from vanilla
// OpenAI-compatible APIs:
//   URL:    {base_url}/chat/completions?api-version={azure_api_version}
//   Auth:   api-key: <key>   (not Authorization: Bearer)
//   Model:  ignored in body — the deployment URL already encodes the model.
//
// base_url must be the full deployment URL:
//   https://{resource}.openai.azure.com/openai/deployments/{deployment}
// ---------------------------------------------------------------------------

class AzureOpenAiAdapter final : public LlmProviderPort {
public:
    explicit AzureOpenAiAdapter(LlmProviderConfig cfg)
        : cfg_(std::move(cfg))
    {
        if (cfg_.base_url.empty())
            throw std::invalid_argument("azure_openai: base_url required");
        if (cfg_.azure_api_version.empty())
            throw std::invalid_argument("azure_openai: azure_api_version required");

        // Extract scheme+host for the HttpClient; path will include the rest.
        // e.g. "https://myres.openai.azure.com/openai/deployments/gpt4"
        //   -> client at "https://myres.openai.azure.com"
        //   -> path "/openai/deployments/gpt4/chat/completions?api-version=..."
        auto slash3 = cfg_.base_url.find("//");
        std::string host_part = cfg_.base_url;
        path_prefix_ = "";
        if (slash3 != std::string::npos) {
            auto path_start = cfg_.base_url.find('/', slash3 + 2);
            if (path_start != std::string::npos) {
                host_part    = cfg_.base_url.substr(0, path_start);
                path_prefix_ = cfg_.base_url.substr(path_start);
            }
        }
        client_ = drogon::HttpClient::newHttpClient(host_part);
        client_->setPipeliningDepth(0);
    }

    drogon::Task<Result<ChatResponse>>
    chat(ChatRequest req, double timeout_s) const override
    {
        auto [body, err] = build_request(req, false);
        if (!err.empty()) co_return std::unexpected(Error::invalid_input(err));

        auto http_req = make_http_request(std::move(body));
        drogon::HttpResponsePtr resp;
        try {
            resp = co_await client_->sendRequestCoro(
                http_req, static_cast<float>(timeout_s));
        } catch (const std::exception& e) {
            co_return std::unexpected(Error::unavailable(
                std::format("llm: azure upstream error: {}", e.what())));
        }
        if (resp->getStatusCode() != drogon::k200OK) {
            spdlog::warn("[llm-azure] HTTP {}: {}",
                static_cast<int>(resp->getStatusCode()), resp->getBody());
            co_return std::unexpected(Error::unavailable(
                std::format("llm: HTTP {}", static_cast<int>(resp->getStatusCode()))));
        }
        OaiResponse oai_resp;
        if (glz::read_json(oai_resp, resp->getBody()))
            co_return std::unexpected(Error::unavailable("llm: invalid response JSON"));
        if (oai_resp.choices.empty())
            co_return std::unexpected(Error::unavailable("llm: empty choices"));
        co_return ChatResponse{
            .content       = oai_resp.choices[0].message.content,
            .input_tokens  = oai_resp.usage.prompt_tokens,
            .output_tokens = oai_resp.usage.completion_tokens,
            .model         = cfg_.model,
            .provider_id   = cfg_.id,
        };
    }

    drogon::Task<Result<ChatResponse>>
    chat_stream(ChatRequest                    req,
                std::function<void(ChatChunk)> on_chunk,
                double                         timeout_s) const override
    {
        auto [body, err] = build_request(req, true);
        if (!err.empty()) co_return std::unexpected(Error::invalid_input(err));

        auto http_req = make_http_request(std::move(body));
        drogon::HttpResponsePtr resp;
        try {
            resp = co_await client_->sendRequestCoro(
                http_req, static_cast<float>(timeout_s));
        } catch (const std::exception& e) {
            co_return std::unexpected(Error::unavailable(
                std::format("llm: azure upstream error: {}", e.what())));
        }
        if (resp->getStatusCode() != drogon::k200OK) {
            co_return std::unexpected(Error::unavailable(
                std::format("llm: HTTP {}", static_cast<int>(resp->getStatusCode()))));
        }

        // SSE parsing identical to OpenAiCompatibleAdapter
        std::string accumulated;
        int input_tokens = 0, output_tokens = 0;
        std::string_view raw = resp->getBody();
        std::size_t pos = 0;
        while (pos < raw.size()) {
            auto nl = raw.find('\n', pos);
            std::string_view line = (nl == std::string_view::npos)
                ? raw.substr(pos) : raw.substr(pos, nl - pos);
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            pos = (nl == std::string_view::npos) ? raw.size() : nl + 1;
            if (line.empty()) continue;
            std::string json_payload;
            if (!parse_sse_line(line, json_payload)) break;
            if (json_payload.empty()) continue;
            OaiStreamChunk chunk;
            if (glz::read_json(chunk, json_payload)) continue;
            if (!chunk.choices.empty() && !chunk.choices[0].delta.content.empty()) {
                accumulated += chunk.choices[0].delta.content;
                on_chunk(ChatChunk{.content = chunk.choices[0].delta.content});
            }
            if (chunk.usage.prompt_tokens > 0)     input_tokens  = chunk.usage.prompt_tokens;
            if (chunk.usage.completion_tokens > 0) output_tokens = chunk.usage.completion_tokens;
        }
        on_chunk(ChatChunk{.done = true});
        co_return ChatResponse{
            .content       = std::move(accumulated),
            .input_tokens  = input_tokens,
            .output_tokens = output_tokens,
            .model         = cfg_.model,
            .provider_id   = cfg_.id,
        };
    }

private:
    LlmProviderConfig                           cfg_;
    std::string                                 path_prefix_;
    mutable std::shared_ptr<drogon::HttpClient> client_;

    std::pair<std::string, std::string>
    build_request(const ChatRequest& req, bool stream) const
    {
        OaiRequest oai;
        // Azure ignores the model field in the body (deployment URL encodes it),
        // but some versions echo it back; send cfg_.model for consistency.
        oai.model       = cfg_.model;
        oai.max_tokens  = (req.max_tokens > 0) ? req.max_tokens : cfg_.max_tokens;
        oai.temperature = (req.temperature >= 0) ? req.temperature : cfg_.temperature;
        oai.stream      = stream;
        if (stream)
            oai.stream_options = OaiStreamOptions{.include_usage = true};
        for (const auto& m : req.messages)
            oai.messages.push_back({.role = m.role, .content = m.content});

        std::string body;
        if (glz::write_json(oai, body)) return {"", "llm: failed to serialise request"};
        return {std::move(body), ""};
    }

    drogon::HttpRequestPtr make_http_request(std::string body) const
    {
        auto req = drogon::HttpRequest::newHttpRequest();
        req->setMethod(drogon::Post);
        req->setPath(path_prefix_ + "/chat/completions?api-version="
                     + cfg_.azure_api_version);
        req->setContentTypeCode(drogon::CT_APPLICATION_JSON);
        req->setBody(std::move(body));
        req->addHeader("api-key", cfg_.api_key);
        return req;
    }
};

} // namespace

// ---------------------------------------------------------------------------
// Public symbols
// ---------------------------------------------------------------------------

std::shared_ptr<LlmProviderPort>
make_openai_compatible_provider(const LlmProviderConfig& cfg)
{
    return std::make_shared<OpenAiCompatibleAdapter>(cfg);
}

std::shared_ptr<LlmProviderPort>
make_azure_openai_provider(const LlmProviderConfig& cfg)
{
    return std::make_shared<AzureOpenAiAdapter>(cfg);
}

} // namespace wikore::rag
