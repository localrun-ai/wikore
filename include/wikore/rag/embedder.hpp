#pragma once
#include "wikore/rag/types.hpp"
#include "wikore/domain/types.hpp"
#include <drogon/HttpClient.h>
#include <drogon/utils/coroutine.h>
#include <memory>
#include <string>
#include <vector>

namespace wikore::rag {

// ---------------------------------------------------------------------------
// EmbedderPort: abstract embedding interface.
//
// Implementations: LlamaEmbedder (HTTP, production), NullEmbedder (tests).
// ---------------------------------------------------------------------------

class EmbedderPort {
public:
    virtual ~EmbedderPort() = default;

    // Embed a single text. Most callers should prefer embed_batch.
    // By-value: Drogon Tasks suspend immediately (suspend_always), so reference
    // params would dangle before the coroutine resumes. Copy into the frame.
    // timeout_s > 0 bounds the HTTP call to that many seconds (callers pass the
    // remaining request deadline); 0 uses the adapter's default cap.
    virtual drogon::Task<Result<Embedding>>
    embed(std::string text, double timeout_s = 0) = 0;

    // Embed multiple texts in one HTTP round-trip.
    // The returned vector preserves input order.
    virtual drogon::Task<Result<std::vector<Embedding>>>
    embed_batch(std::vector<std::string> texts) = 0;

    virtual int dims() const = 0;
    virtual const std::string& model_name() const = 0;
};

// ---------------------------------------------------------------------------
// LlamaEmbedder: adapter for OpenAI-compatible /v1/embeddings endpoint
// (llama-server, text-embeddings-inference, cloud APIs).
//
// A single persistent HttpClientPtr is held per instance; Drogon's client
// is thread-safe and pipelines requests on a keep-alive connection.
// Construct once at startup and share the pointer wherever embedding is needed.
// ---------------------------------------------------------------------------

class LlamaEmbedder : public EmbedderPort {
public:
    LlamaEmbedder(std::string base_url, std::string model, int dims);

    drogon::Task<Result<Embedding>>
    embed(std::string text, double timeout_s = 0) override;

    drogon::Task<Result<std::vector<Embedding>>>
    embed_batch(std::vector<std::string> texts) override;

    int dims() const override { return _dims; }
    const std::string& model_name() const override { return _model; }

private:
    std::string           _base_url;
    std::string           _model;
    int                   _dims;
    drogon::HttpClientPtr _client;

    drogon::Task<Result<std::vector<Embedding>>>
    do_embed(std::vector<std::string> texts, double timeout_s = 0);
};

// ---------------------------------------------------------------------------
// NullEmbedder: returns deterministic fixed-value embeddings for testing.
//
// Each distinct input string maps to a stable unit vector seeded from the
// string's hash, so test assertions on embedding values are reproducible
// without a running llama-server.
// ---------------------------------------------------------------------------

class NullEmbedder : public EmbedderPort {
public:
    explicit NullEmbedder(int dims = 4) : _dims(dims) {}

    drogon::Task<Result<Embedding>> embed(std::string text, double timeout_s = 0) override;
    drogon::Task<Result<std::vector<Embedding>>>
    embed_batch(std::vector<std::string> texts) override;

    int dims() const override { return _dims; }
    const std::string& model_name() const override { return _name; }

private:
    int         _dims;
    std::string _name = "null-embedder";
};

} // namespace wikore::rag
