#include "wikore/rag/embedder.hpp"
#include <glaze/glaze.hpp>
#include <drogon/HttpRequest.h>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <stdexcept>

namespace wikore::rag {

// Per-request timeout (seconds) for the embed HTTP call. Without it,
// sendRequestCoro waits forever, so a stalled llama-server would hang every
// caller indefinitely (a request-deadline check between steps cannot interrupt
// an in-flight await). On timeout the coro throws, which the send site below
// converts to Error::unavailable -> HTTP 503.
static constexpr double kEmbedTimeoutSecs = 30.0;

// ---------------------------------------------------------------------------
// JSON schemas for OpenAI-compatible /v1/embeddings
// ---------------------------------------------------------------------------

struct EmbedRequest {
    std::string              model;
    std::vector<std::string> input;
};

struct EmbedData {
    std::vector<float> embedding;
    int                index = 0;
};

struct EmbedResponse {
    std::vector<EmbedData> data;
};

// ---------------------------------------------------------------------------
// LlamaEmbedder
// ---------------------------------------------------------------------------

LlamaEmbedder::LlamaEmbedder(std::string base_url, std::string model, int dims)
    : _base_url(std::move(base_url)), _model(std::move(model)), _dims(dims)
{
    _client = drogon::HttpClient::newHttpClient(_base_url);
    _client->setPipeliningDepth(4);
}

drogon::Task<Result<Embedding>>
LlamaEmbedder::embed(std::string text, double timeout_s)
{
    // Avoid braced-init-list + std::move on coroutine param: GCC 13 ICE in
    // build_special_member_call. Build the vector explicitly instead.
    std::vector<std::string> batch;
    batch.push_back(std::move(text));
    auto result = co_await do_embed(std::move(batch), timeout_s);
    if (!result)
        co_return std::unexpected(result.error());
    if (result->empty())
        co_return std::unexpected(Error::unavailable("empty embedding response"));
    co_return std::move((*result)[0]);
}

drogon::Task<Result<std::vector<Embedding>>>
LlamaEmbedder::embed_batch(std::vector<std::string> texts)
{
    co_return co_await do_embed(std::move(texts));
}

drogon::Task<Result<std::vector<Embedding>>>
LlamaEmbedder::do_embed(std::vector<std::string> texts, double timeout_s)
{
    EmbedRequest body{_model, texts};
    std::string  json{};
    if (auto err = glz::write_json(body, json); err) {
        co_return std::unexpected(Error::invalid_input("embed request serialize failed"));
    }

    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Post);
    req->setPath("/v1/embeddings");
    req->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    req->setBody(std::move(json));

    drogon::HttpResponsePtr resp;
    try {
        // Bound the call to the caller's remaining deadline when given, but
        // never above the hard cap.
        const double eff = timeout_s > 0.0 ? std::min(timeout_s, kEmbedTimeoutSecs)
                                           : kEmbedTimeoutSecs;
        resp = co_await _client->sendRequestCoro(req, eff);
    } catch (const std::exception& ex) {
        co_return std::unexpected(
            Error::unavailable(std::format("embed HTTP error: {}", ex.what())));
    }

    if (resp->getStatusCode() != drogon::k200OK) {
        co_return std::unexpected(Error::unavailable(
            std::format("embed server {}", static_cast<int>(resp->getStatusCode()))));
    }

    EmbedResponse parsed{};
    if (auto err = glz::read_json(parsed, resp->getBody()); err) {
        co_return std::unexpected(
            Error::unavailable("embed response parse failed"));
    }

    if (parsed.data.size() != texts.size()) {
        co_return std::unexpected(Error::unavailable(std::format(
            "embed response cardinality: expected {} embeddings, got {}",
            texts.size(), parsed.data.size())));
    }

    std::sort(parsed.data.begin(), parsed.data.end(),
              [](const EmbedData& a, const EmbedData& b) { return a.index < b.index; });

    // Indices must form the contiguous range [0..N-1] -- otherwise we'd
    // align embeddings to the wrong texts via positional access.
    for (std::size_t i = 0; i < parsed.data.size(); ++i) {
        if (parsed.data[i].index != static_cast<int>(i)) {
            co_return std::unexpected(Error::unavailable(std::format(
                "embed response indices not [0..{}]: position {} has index {}",
                parsed.data.size() - 1, i, parsed.data[i].index)));
        }
        if (static_cast<int>(parsed.data[i].embedding.size()) != _dims) {
            co_return std::unexpected(Error::unavailable(std::format(
                "embed response dim mismatch at index {}: expected {}, got {}",
                i, _dims, parsed.data[i].embedding.size())));
        }
    }

    std::vector<Embedding> out;
    out.reserve(parsed.data.size());
    for (auto& d : parsed.data)
        out.push_back(std::move(d.embedding));
    co_return out;
}

// ---------------------------------------------------------------------------
// NullEmbedder
// ---------------------------------------------------------------------------

static Embedding make_null_embedding(const std::string& text, int dims)
{
    // FNV-1a 64-bit hash to seed a deterministic unit vector.
    uint64_t h = 14695981039346656037ULL;
    for (unsigned char c : text) {
        h ^= static_cast<uint64_t>(c);
        h *= 1099511628211ULL;
    }

    Embedding emb(dims);
    float     sum_sq = 0.0f;
    for (int i = 0; i < dims; ++i) {
        h        = h * 6364136223846793005ULL + 1442695040888963407ULL;
        emb[i]   = static_cast<float>(static_cast<int64_t>(h))
                   / static_cast<float>(INT64_MAX);
        sum_sq  += emb[i] * emb[i];
    }
    float norm = std::sqrt(sum_sq);
    if (norm > 0.0f)
        for (auto& v : emb) v /= norm;
    return emb;
}

drogon::Task<Result<Embedding>>
NullEmbedder::embed(std::string text, double /*timeout_s*/)
{
    co_return make_null_embedding(text, _dims);
}

drogon::Task<Result<std::vector<Embedding>>>
NullEmbedder::embed_batch(std::vector<std::string> texts)
{
    std::vector<Embedding> out;
    out.reserve(texts.size());
    for (const auto& t : texts)
        out.push_back(make_null_embedding(t, _dims));
    co_return out;
}

} // namespace wikore::rag
