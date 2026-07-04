// ---------------------------------------------------------------------------
// AnthropicAdapter — POST https://api.anthropic.com/v1/messages
//
// Anthropic's Messages API differs from OpenAI-compat in three ways:
//   1. Auth header: "x-api-key: <key>" instead of "Authorization: Bearer".
//   2. Request body: "system" message is a top-level field, not in messages[].
//   3. Streaming: uses event:content_block_delta / delta.text instead of
//      choices[0].delta.content.
//
// API version header "anthropic-version: 2023-06-01" is required.
// ---------------------------------------------------------------------------

#include "wikore/rag/llm_provider.hpp"
#include <drogon/HttpClient.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <glaze/glaze.hpp>
#include <spdlog/spdlog.h>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wikore::rag {

namespace {

static constexpr std::string_view kAnthropicBaseUrl = "https://api.anthropic.com";
static constexpr std::string_view kAnthropicVersion = "2023-06-01";

// ---------------------------------------------------------------------------
// JSON schemas
// ---------------------------------------------------------------------------

struct AnthropicMessage {
    std::string role;    // "user" | "assistant"
    std::string content;
};

struct AnthropicRequest {
    std::string                    model;
    std::vector<AnthropicMessage>  messages;
    std::optional<std::string>     system;
    int                            max_tokens  = 2048;
    float                          temperature = 0.1f;
    bool                           stream      = false;
};

// Non-streaming response
struct AnthropicContentBlock {
    std::string type;
    std::string text;
};
struct AnthropicUsage {
    int input_tokens  = 0;
    int output_tokens = 0;
};
struct AnthropicResponse {
    std::vector<AnthropicContentBlock> content;
    AnthropicUsage                     usage;
    std::string                        model;
    std::string                        id;
};

// Streaming: event types we care about
//   content_block_delta -> delta.type=="text_delta" -> delta.text
//   message_delta       -> usage.output_tokens
//   message_start       -> message.usage.input_tokens
struct AnthropicDelta {
    std::string type;  // "text_delta"
    std::string text;
};
struct AnthropicStreamEvent {
    std::string    type;    // "content_block_delta", "message_delta", "message_start"
    AnthropicDelta delta;
    // message_start embeds usage in .message.usage
    struct MsgWrapper {
        AnthropicUsage usage;
    };
    MsgWrapper message;
    // message_delta
    AnthropicUsage usage;
};

// ---------------------------------------------------------------------------
// AnthropicAdapter
// ---------------------------------------------------------------------------

class AnthropicAdapter final : public LlmProviderPort {
public:
    explicit AnthropicAdapter(LlmProviderConfig cfg)
        : cfg_(std::move(cfg))
    {
        std::string url = cfg_.base_url.empty()
            ? std::string(kAnthropicBaseUrl)
            : cfg_.base_url;
        client_ = drogon::HttpClient::newHttpClient(url);
        client_->setPipeliningDepth(0);
    }

    drogon::Task<Result<ChatResponse>>
    chat(ChatRequest req, double timeout_s) const override
    {
        auto [body, err] = build_request(req, /*stream=*/false);
        if (!err.empty()) co_return std::unexpected(Error::invalid_input(err));

        auto http_req = make_http_request(std::move(body));

        drogon::HttpResponsePtr resp;
        try {
            resp = co_await client_->sendRequestCoro(
                http_req, static_cast<float>(timeout_s));
        } catch (const std::exception& e) {
            spdlog::warn("[llm-anthropic] request failed: {}", e.what());
            co_return std::unexpected(Error::unavailable(
                std::format("llm: upstream error: {}", e.what())));
        }

        if (resp->getStatusCode() != drogon::k200OK) {
            spdlog::warn("[llm-anthropic] HTTP {}: {}",
                static_cast<int>(resp->getStatusCode()), resp->getBody());
            co_return std::unexpected(Error::unavailable(
                std::format("llm: HTTP {}", static_cast<int>(resp->getStatusCode()))));
        }

        AnthropicResponse ar;
        if (glz::read_json(ar, resp->getBody())) {
            spdlog::warn("[llm-anthropic] JSON parse error");
            co_return std::unexpected(Error::unavailable("llm: invalid response JSON"));
        }
        if (ar.content.empty()) {
            co_return std::unexpected(Error::unavailable("llm: empty content in response"));
        }
        // Concatenate all text blocks (usually just one).
        std::string text;
        for (const auto& b : ar.content)
            if (b.type == "text") text += b.text;

        co_return ChatResponse{
            .content       = std::move(text),
            .input_tokens  = ar.usage.input_tokens,
            .output_tokens = ar.usage.output_tokens,
            .model         = ar.model.empty() ? cfg_.model : ar.model,
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

        auto http_req = make_http_request(std::move(body));

        drogon::HttpResponsePtr resp;
        try {
            resp = co_await client_->sendRequestCoro(
                http_req, static_cast<float>(timeout_s));
        } catch (const std::exception& e) {
            spdlog::warn("[llm-anthropic] stream request failed: {}", e.what());
            co_return std::unexpected(Error::unavailable(
                std::format("llm: upstream error: {}", e.what())));
        }

        if (resp->getStatusCode() != drogon::k200OK) {
            spdlog::warn("[llm-anthropic] stream HTTP {}",
                         static_cast<int>(resp->getStatusCode()));
            co_return std::unexpected(Error::unavailable(
                std::format("llm: HTTP {}", static_cast<int>(resp->getStatusCode()))));
        }

        std::string accumulated;
        int input_tokens  = 0;
        int output_tokens = 0;
        std::string model_echo;
        bool message_stop_received = false; // Anthropic signals completion via message_stop

        // Parse Anthropic SSE: "event: <type>\ndata: <json>\n\n"
        std::string_view raw = resp->getBody();
        std::size_t pos = 0;
        std::string current_event;

        while (pos < raw.size()) {
            auto nl = raw.find('\n', pos);
            std::string_view line = (nl == std::string_view::npos)
                ? raw.substr(pos)
                : raw.substr(pos, nl - pos);
            if (!line.empty() && line.back() == '\r')
                line.remove_suffix(1);
            pos = (nl == std::string_view::npos) ? raw.size() : nl + 1;

            if (line.starts_with("event:")) {
                current_event = line.substr(6);
                if (!current_event.empty() && current_event[0] == ' ')
                    current_event = current_event.substr(1);
                if (current_event == "message_stop")
                    message_stop_received = true;
                continue;
            }
            if (line.starts_with("data:")) {
                auto payload = line.substr(5);
                if (!payload.empty() && payload[0] == ' ')
                    payload.remove_prefix(1);
                if (payload == "[DONE]") break;

                // Anthropic error event: {"type":"error","error":{...}}
                if (current_event == "error") {
                    spdlog::warn("[llm-anthropic] upstream error event: {}",
                                 std::string(payload));
                    co_return std::unexpected(
                        Error::unavailable("llm: upstream returned error in stream"));
                }

                AnthropicStreamEvent ev;
                if (glz::read_json(ev, std::string(payload))) continue;

                if (current_event == "content_block_delta"
                    && ev.delta.type == "text_delta"
                    && !ev.delta.text.empty()) {
                    accumulated += ev.delta.text;
                    on_chunk(ChatChunk{.content = ev.delta.text, .done = false});
                } else if (current_event == "message_start") {
                    input_tokens = ev.message.usage.input_tokens;
                } else if (current_event == "message_delta") {
                    output_tokens = ev.usage.output_tokens;
                }
                continue;
            }
            // Empty line: event boundary — reset current event
            if (line.empty()) current_event.clear();
        }

        if (!message_stop_received) {
            spdlog::warn("[llm-anthropic] stream ended without message_stop event");
            co_return std::unexpected(
                Error::unavailable("llm: incomplete stream (no message_stop received)"));
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
    mutable std::shared_ptr<drogon::HttpClient> client_;

    std::pair<std::string, std::string>
    build_request(const ChatRequest& req, bool stream) const
    {
        AnthropicRequest ar;
        ar.model       = req.model.empty() ? cfg_.model : req.model;
        ar.max_tokens  = req.max_tokens  > 0 ? req.max_tokens  : cfg_.max_tokens;
        ar.temperature = req.temperature >= 0 ? req.temperature : cfg_.temperature;
        ar.stream      = stream;
        if (ar.model.empty()) return {"", "llm: no model configured"};

        // Separate the optional system message from the conversation turns.
        for (const auto& m : req.messages) {
            if (m.role == "system") {
                ar.system = m.content;
            } else {
                ar.messages.push_back({.role = m.role, .content = m.content});
            }
        }

        std::string body;
        if (glz::write_json(ar, body)) return {"", "llm: failed to serialise request"};
        return {std::move(body), ""};
    }

    drogon::HttpRequestPtr make_http_request(std::string body) const
    {
        auto req = drogon::HttpRequest::newHttpRequest();
        req->setMethod(drogon::Post);
        req->setPath("/v1/messages");
        req->setContentTypeCode(drogon::CT_APPLICATION_JSON);
        req->setBody(std::move(body));
        req->addHeader("x-api-key", cfg_.api_key);
        req->addHeader("anthropic-version", std::string(kAnthropicVersion));
        return req;
    }
};

} // namespace

std::shared_ptr<LlmProviderPort>
make_anthropic_provider(const LlmProviderConfig& cfg)
{
    return std::make_shared<AnthropicAdapter>(cfg);
}

} // namespace wikore::rag
