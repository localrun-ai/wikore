-- V033: LLM provider configurations
--
-- Admin-managed table that stores one or more LLM provider configs per
-- company (or a system-wide default when company_id IS NULL).
--
-- Provider types:
--   openai_compatible  — anything speaking POST /v1/chat/completions with
--                        OpenAI-format SSE: llama.cpp, Ollama, vLLM, OpenAI,
--                        Groq, Together.ai, Mistral, Perplexity, Azure OpenAI.
--   anthropic          — Anthropic Messages API (POST /v1/messages).
--   gemini             — Google Gemini REST API.
--
-- credentials / credentials_key_id follow the integrations table pattern
-- (V006 + V027): the API key is encrypted with AES-256-GCM; the key
-- material is never stored in the DB.  Local providers (llama.cpp, Ollama)
-- leave credentials NULL.
--
-- is_default: at most one default per (company_id) scope, enforced by the
-- partial unique index below.  The API server picks the default provider
-- when no explicit provider_id is specified on a chat request.

CREATE TABLE llm_providers (
    id                  UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    -- NULL = system-wide default available to all companies
    company_id          UUID        REFERENCES companies(id) ON DELETE CASCADE,
    provider            TEXT        NOT NULL
                            CHECK (provider IN (
                                'openai_compatible',
                                'anthropic',
                                'gemini'
                            )),
    display_name        TEXT        NOT NULL,
    -- base_url required for openai_compatible; NULL for anthropic/gemini
    -- (SDK default endpoints are used).
    base_url            TEXT,
    model               TEXT        NOT NULL,
    max_tokens          INT         NOT NULL DEFAULT 2048
                            CHECK (max_tokens BETWEEN 1 AND 131072),
    temperature         NUMERIC(4,3) NOT NULL DEFAULT 0.100
                            CHECK (temperature BETWEEN 0 AND 2),
    -- Encrypted API key blob (base64-encoded AES-256-GCM ciphertext).
    -- NULL for local/unauthenticated providers (llama.cpp, Ollama without auth).
    credentials         TEXT,
    credentials_key_id  TEXT,
    credentials_alg     TEXT        NOT NULL DEFAULT 'aes-256-gcm',
    is_default          BOOLEAN     NOT NULL DEFAULT false,
    enabled             BOOLEAN     NOT NULL DEFAULT true,
    created_by          UUID        REFERENCES users(id) ON DELETE SET NULL,
    created_at          TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at          TIMESTAMPTZ NOT NULL DEFAULT now(),
    CONSTRAINT llm_providers_credentials_key_id_consistent_chk
        CHECK ((credentials IS NULL) = (credentials_key_id IS NULL))
);

-- At most one default per (company_id) scope.
-- COALESCE maps NULL company_id to a sentinel UUID so the expression is
-- always non-null and the index covers both system-wide and per-company rows.
CREATE UNIQUE INDEX llm_providers_one_default_per_scope_idx
    ON llm_providers (COALESCE(company_id, '00000000-0000-0000-0000-000000000000'::uuid))
    WHERE is_default AND enabled;

CREATE INDEX llm_providers_company_idx ON llm_providers (company_id);

CREATE TRIGGER llm_providers_updated_at
    BEFORE UPDATE ON llm_providers
    FOR EACH ROW EXECUTE FUNCTION set_updated_at();

COMMENT ON TABLE llm_providers IS
    'Admin-managed LLM backend configurations. Supports OpenAI-compatible '
    'local/cloud providers, Anthropic, and Gemini. Credentials are '
    'encrypted with AES-256-GCM (same scheme as integrations table).';
