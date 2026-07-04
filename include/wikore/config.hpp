#pragma once
#include <string>
#include <cstdlib>

namespace wikore {

struct Config {
    // PostgreSQL
    std::string database_url   = "postgresql://wikore:wikore@localhost:5432/wikore";
    std::string partition_database_url;

    // Redis
    std::string redis_url      = "redis://127.0.0.1:6379/0";

    // Qdrant
    std::string qdrant_url     = "http://localhost:6333";

    // LLM (OpenAI-compatible: llama-server or cloud)
    std::string llm_base_url   = "http://localhost:8080/v1";
    std::string llm_model;
    int         llm_max_tokens = 2048;
    float       llm_temperature = 0.1f;
    // Per-tenant LLM guardrails (enforced in Redis; see rag::LlmGate).
    int         llm_concurrency   = 4;      // max in-flight LLM calls per tenant
    double      llm_rate_per_sec  = 5.0;    // sustained request rate per tenant
    int         llm_rate_burst    = 15;     // token-bucket capacity (burst) per tenant

    // Embeddings
    std::string embed_base_url = "http://localhost:8081/v1";
    std::string embed_model;
    int         embed_dims     = 768;

    // OIDC / SSO
    std::string oidc_issuer;               // e.g. https://auth.example.com/realms/wikore
    std::string oidc_audience  = "wikore";  // expected 'aud' claim

    // AES-256-GCM key for encrypting integration credentials (hex, 64 chars)
    std::string credentials_key;

    // Server
    int         port           = 9000;
    std::string allowed_origin = "*";

    // Optional: Anthropic (for wiki ingest if not using local LLM)
    std::string anthropic_api_key;
    std::string anthropic_model = "claude-sonnet-4-6";

    static Config from_env() {
        Config c;
        auto e = [](const char* k, std::string& v) {
            if (const char* s = std::getenv(k)) v = s;
        };
        auto ei = [](const char* k, int& v) {
            if (const char* s = std::getenv(k)) v = std::stoi(s);
        };
        auto ef = [](const char* k, double& v) {
            if (const char* s = std::getenv(k)) v = std::stod(s);
        };
        e("DATABASE_URL",      c.database_url);
        e("PARTITION_DATABASE_URL", c.partition_database_url);
        e("REDIS_URL",         c.redis_url);
        e("QDRANT_URL",        c.qdrant_url);
        e("LLM_BASE_URL",      c.llm_base_url);
        e("LLM_MODEL",         c.llm_model);
        e("EMBED_BASE_URL",    c.embed_base_url);
        e("EMBED_MODEL",       c.embed_model);
        e("OIDC_ISSUER",       c.oidc_issuer);
        e("OIDC_AUDIENCE",     c.oidc_audience);
        e("CREDENTIALS_KEY",   c.credentials_key);
        e("ALLOWED_ORIGIN",    c.allowed_origin);
        e("ANTHROPIC_API_KEY", c.anthropic_api_key);
        e("ANTHROPIC_MODEL",   c.anthropic_model);
        ei("PORT",             c.port);
        ei("LLM_MAX_TOKENS",   c.llm_max_tokens);
        ei("LLM_CONCURRENCY",  c.llm_concurrency);
        ei("LLM_RATE_BURST",   c.llm_rate_burst);
        ef("LLM_RATE_PER_SEC", c.llm_rate_per_sec);
        ei("EMBED_DIMS",       c.embed_dims);
        return c;
    }
};

} // namespace wikore
