# Wikore - Schema and Iteration 1 Code Review

You are reviewing **Wikore**: a self-hosted, permission-aware enterprise wiki
and RAG backend. Single binary per deployment. Stack: C++23, Drogon (async
HTTP + coroutines), PostgreSQL 17, Qdrant (vector index), Redis.

**Mission**: find real issues, not style nits. Focus on correctness, safety,
and contract violations. If something looks intentional and correct, say so
briefly. If something is wrong or risky, explain why and what the fix is.

---

## Part 1 - PostgreSQL Schema (V001-V028)

### Key design patterns (apply to every table)

- **Composite FKs**: every cross-table reference uses
  `FOREIGN KEY (company_id, other_id) REFERENCES target(company_id, id)`
  so same-company membership is enforced at the DB, not just in application
  code. Every referenced table has `UNIQUE (company_id, id)`.
- **Hard tenant boundary**: `companies` table. All entities carry `company_id`.
- **Lifecycle state machine** for `document_versions` and `wiki_page_versions`:
  `draft -> active -> deprecated -> archived` (archived is terminal).
  `promote_document_version()` (V010) and `promote_wiki_page_version()` (V022)
  are the only correct write paths for activation. Both use FOR UPDATE on the
  parent row and deprecate-first / activate-second ordering to satisfy a
  non-deferrable partial unique index.
- **Append-only tables** (audit_log, usage_events, outbox_events,
  document_chunk_tombstones, wiki_page_version_tombstones): guarded by
  BEFORE UPDATE/DELETE triggers that raise exceptions.
- **Transactional outbox** (V015): every Qdrant or Redis side-effect is
  recorded in `outbox_events` inside the same Postgres transaction as the
  state change. The scheduler drains these; no HTTP call ever happens
  inside a use case.

### Schema migrations (abridged - key SQL included)

**V001 - companies + org_unit_closure**

```sql
CREATE TABLE companies (
    id          UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    name        TEXT NOT NULL,
    slug        TEXT NOT NULL UNIQUE,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE (id)  -- allows composite FK targets
);

CREATE TABLE org_units (
    id          UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    company_id  UUID NOT NULL,
    parent_id   UUID,              -- NULL for root
    type        TEXT NOT NULL CHECK (type IN ('root','division','department','team')),
    name        TEXT NOT NULL,
    slug        TEXT NOT NULL,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE (company_id, id),
    CONSTRAINT org_units_company_fk FOREIGN KEY (company_id)
        REFERENCES companies(id) ON DELETE CASCADE,
    CONSTRAINT org_units_parent_fk FOREIGN KEY (company_id, parent_id)
        REFERENCES org_units(company_id, id) ON DELETE RESTRICT
);

-- Transitive closure: one row per (ancestor, descendant) pair including self.
-- depth=0 is self-reference. Used for access scope resolution.
CREATE TABLE org_unit_closure (
    ancestor_id   UUID NOT NULL REFERENCES org_units(id) ON DELETE CASCADE,
    descendant_id UUID NOT NULL REFERENCES org_units(id) ON DELETE CASCADE,
    depth         INT  NOT NULL DEFAULT 0,
    PRIMARY KEY (ancestor_id, descendant_id)
);
```

**V002 - users, auth, api_keys, memberships, groups, resource_grants**

```sql
CREATE TABLE users (
    id              UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    company_id      UUID NOT NULL,
    external_issuer TEXT NOT NULL,       -- OIDC issuer URL
    external_sub    TEXT NOT NULL,       -- OIDC sub claim
    email           TEXT NOT NULL,
    display_name    TEXT NOT NULL DEFAULT '',
    avatar_url      TEXT,
    is_admin        BOOLEAN NOT NULL DEFAULT false,
    created_at      TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at      TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE (company_id, id),
    UNIQUE (company_id, external_issuer, external_sub),
    CONSTRAINT users_company_fk FOREIGN KEY (company_id)
        REFERENCES companies(id) ON DELETE CASCADE
);

CREATE TABLE api_keys (
    id          UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    company_id  UUID NOT NULL,
    user_id     UUID NOT NULL,
    key_hash    TEXT NOT NULL UNIQUE,  -- SHA-256 of the raw key
    name        TEXT NOT NULL,
    last_used   TIMESTAMPTZ,
    expires_at  TIMESTAMPTZ,
    revoked_at  TIMESTAMPTZ,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    CONSTRAINT api_keys_user_same_company_fk
        FOREIGN KEY (company_id, user_id)
        REFERENCES users(company_id, id) ON DELETE CASCADE
);

CREATE TABLE memberships (
    id          UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    company_id  UUID NOT NULL,
    user_id     UUID,
    group_id    UUID,
    org_unit_id UUID NOT NULL,
    role        TEXT NOT NULL CHECK (role IN ('viewer','editor','admin')),
    applies_to  TEXT NOT NULL CHECK (applies_to IN ('self_only','self_and_descendants')),
    granted_by  UUID,
    granted_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    CONSTRAINT memberships_org_unit_same_company_fk
        FOREIGN KEY (company_id, org_unit_id)
        REFERENCES org_units(company_id, id) ON DELETE CASCADE,
    -- either user_id or group_id, not both, not neither:
    CONSTRAINT memberships_principal_chk CHECK (
        (user_id IS NOT NULL) != (group_id IS NOT NULL))
);

CREATE TABLE resource_grants (
    id                   UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    company_id           UUID NOT NULL,
    resource_type        TEXT NOT NULL CHECK (resource_type IN ('org_unit','document','wiki_page')),
    resource_id          UUID NOT NULL,       -- polymorphic; no FK
    principal_type       TEXT NOT NULL CHECK (principal_type IN ('org_unit')),  -- MVP: org_unit only
    principal_id         UUID NOT NULL,
    permission           TEXT NOT NULL CHECK (permission IN ('read','write','admin')),
    resource_applies_to  TEXT NOT NULL CHECK (resource_applies_to IN ('self_only','self_and_descendants')),
    principal_applies_to TEXT NOT NULL CHECK (principal_applies_to IN ('self_only','self_and_descendants')),
    granted_by           UUID,
    granted_at           TIMESTAMPTZ NOT NULL DEFAULT now(),
    expires_at           TIMESTAMPTZ,
    CONSTRAINT resource_grants_company_fk FOREIGN KEY (company_id)
        REFERENCES companies(id) ON DELETE CASCADE
);
```

V009 adds a BEFORE INSERT OR UPDATE trigger `validate_resource_grant_same_company`
that validates the polymorphic `resource_id` exists in the correct table and
belongs to the same company. `principal_id` is validated as an org_unit.

**V003 - documents, document_versions, document_sections, document_chunks**

```sql
CREATE TABLE documents (
    id          UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    company_id  UUID NOT NULL,
    org_unit_id UUID NOT NULL,
    title       TEXT NOT NULL,
    source_url  TEXT,
    created_by  UUID,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE (company_id, id),
    CONSTRAINT documents_org_unit_same_company_fk
        FOREIGN KEY (company_id, org_unit_id)
        REFERENCES org_units(company_id, id) ON DELETE CASCADE
);

CREATE TABLE document_versions (
    id               UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    company_id       UUID NOT NULL,
    document_id      UUID NOT NULL,
    version_number   INT  NOT NULL,
    file_path        TEXT NOT NULL,
    file_hash        TEXT NOT NULL,           -- SHA-256 of raw bytes
    mime_type        TEXT NOT NULL,
    lifecycle_status TEXT NOT NULL DEFAULT 'draft'
                     CHECK (lifecycle_status IN ('draft','active','deprecated','archived')),
    ingest_status    TEXT NOT NULL DEFAULT 'pending'
                     CHECK (ingest_status IN ('pending','processing','done','error')),
    activated_at     TIMESTAMPTZ,
    superseded_at    TIMESTAMPTZ,
    completed_at     TIMESTAMPTZ,             -- set when ingest_status='done'
    chunk_count      INT,
    error_msg        TEXT,
    created_by       UUID,
    created_at       TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE (company_id, id),
    -- Exactly one active version per document at any time:
    CONSTRAINT document_versions_one_active_per_doc_uidx
        UNIQUE NULLS NOT DISTINCT (document_id, lifecycle_status)
        WHERE (lifecycle_status = 'active'),
    -- 'done' requires both completed_at and chunk_count:
    CONSTRAINT document_versions_done_state_chk CHECK (
        ingest_status <> 'done'
        OR (completed_at IS NOT NULL AND chunk_count IS NOT NULL)),
    CONSTRAINT document_versions_doc_same_company_fk
        FOREIGN KEY (company_id, document_id)
        REFERENCES documents(company_id, id) ON DELETE CASCADE
);

CREATE TABLE document_sections (
    id               UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    company_id       UUID NOT NULL,
    document_version_id UUID NOT NULL,
    parent_id        UUID,
    heading          TEXT NOT NULL DEFAULT '',
    heading_path     TEXT[] NOT NULL DEFAULT '{}',
    depth            INT NOT NULL DEFAULT 0,
    ordinal          INT NOT NULL DEFAULT 0,
    UNIQUE (company_id, id),
    CONSTRAINT document_sections_version_same_company_fk
        FOREIGN KEY (company_id, document_version_id)
        REFERENCES document_versions(company_id, id) ON DELETE CASCADE,
    CONSTRAINT document_sections_parent_fk
        FOREIGN KEY (company_id, parent_id)
        REFERENCES document_sections(company_id, id) ON DELETE CASCADE
);

CREATE TABLE document_chunks (
    id                   UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    company_id           UUID NOT NULL,
    document_version_id  UUID NOT NULL,
    section_id           UUID,
    chunk_index          INT  NOT NULL,
    text                 TEXT NOT NULL,
    access_scope_ids     UUID[] NOT NULL DEFAULT '{}',  -- denormalized for Qdrant resync
    section_heading      TEXT,
    created_at           TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE (company_id, id),
    UNIQUE (document_version_id, chunk_index),          -- idempotent upsert target
    CONSTRAINT document_chunks_version_same_company_fk
        FOREIGN KEY (company_id, document_version_id)
        REFERENCES document_versions(company_id, id) ON DELETE CASCADE
);
-- GIN index on access_scope_ids (UUID[]) for Qdrant resync worker
CREATE INDEX document_chunks_scopes_idx
    ON document_chunks USING GIN (access_scope_ids);
```

V014 adds:
- `documents.deleted_at TIMESTAMPTZ` (soft-delete, Qdrant sync watches)
- `document_chunk_tombstones` (append-only, content_hash proof, no FK)

**V004 - wiki_pages, wiki_page_versions, wiki_page_sources**

Mirrors the document model. `wiki_page_versions` have a `content TEXT` column
(not chunked at ingest; synthesis happens at query time). V022 adds lifecycle
CHECK constraints, timestamp trigger, and `promote_wiki_page_version()` that
mirrors V010's `promote_document_version()`.

**V005 - chat_sessions, chat_turns**

```sql
CREATE TABLE chat_sessions (
    id          UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    company_id  UUID NOT NULL,
    org_unit_id UUID NOT NULL,
    user_id     UUID NOT NULL,
    title       TEXT,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE (company_id, id),
    CONSTRAINT chat_sessions_org_unit_same_company_fk
        FOREIGN KEY (company_id, org_unit_id)
        REFERENCES org_units(company_id, id) ON DELETE CASCADE,
    CONSTRAINT chat_sessions_user_same_company_fk
        FOREIGN KEY (company_id, user_id)
        REFERENCES users(company_id, id) ON DELETE CASCADE
);

CREATE TABLE chat_turns (
    id                  UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    company_id          UUID NOT NULL,
    chat_session_id     UUID NOT NULL,
    question            TEXT NOT NULL,
    answer              TEXT,
    rag_sources         JSONB NOT NULL DEFAULT '[]',  -- CHECK: must be array
    tool_calls          JSONB NOT NULL DEFAULT '[]',  -- CHECK: must be array
    -- Immutable snapshot of org_unit_ids that were searched for this turn:
    access_scope_ids    UUID[] NOT NULL DEFAULT '{}',
    created_at          TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE (company_id, id),
    CONSTRAINT chat_turns_session_same_company_fk
        FOREIGN KEY (company_id, chat_session_id)
        REFERENCES chat_sessions(company_id, id) ON DELETE CASCADE,
    CONSTRAINT chat_turns_rag_sources_array_chk CHECK (jsonb_typeof(rag_sources) = 'array'),
    CONSTRAINT chat_turns_tool_calls_array_chk  CHECK (jsonb_typeof(tool_calls)  = 'array')
);
```

V018 adds `chat_turn_feedback` and `chunk_quality_signals`.
V021 adds `chat_turns.parent_turn_id` (simple self-FK, not composite; same-session
is an application invariant, acknowledged in the migration comment).
V023 adds `shared_chat_threads` with `access_scope_snapshot UUID[]` (immutable),
guarded by an immutability trigger.
V028 adds `CREATE INDEX ... USING GIN (access_scope_ids)` on `chat_turns`.

**V006 - integrations, mcp_tools**

`integrations` has `credentials TEXT` (AES-256-GCM encrypted blob),
`credentials_key_id TEXT` (key version identifier), `credentials_alg TEXT`.
V027 adds a CHECK that credentials and credentials_key_id move together
(both NULL or both non-NULL).

**V007 - audit_log**

Append-only. Partitioned by quarter (`audit_log_2026_q2` through
`audit_log_2027_q2`, plus `audit_log_default` catch-all). No FKs (see comment).
BEFORE UPDATE/DELETE triggers raise exception.

**V010 - promote_document_version() + lifecycle timestamp trigger**

```sql
CREATE OR REPLACE FUNCTION promote_document_version(
    p_company_id UUID, p_document_id UUID, p_version_id UUID)
RETURNS VOID LANGUAGE plpgsql AS $$
DECLARE v_now TIMESTAMPTZ := clock_timestamp();
BEGIN
    PERFORM 1 FROM documents
    WHERE company_id = p_company_id AND id = p_document_id
    FOR UPDATE;
    IF NOT FOUND THEN
        RAISE EXCEPTION 'Document % not found in company %', p_document_id, p_company_id;
    END IF;

    PERFORM 1 FROM document_versions
    WHERE company_id = p_company_id AND document_id = p_document_id
      AND id = p_version_id AND ingest_status = 'done'
      AND lifecycle_status <> 'archived';
    IF NOT FOUND THEN
        RAISE EXCEPTION 'Version % is not promotable ...', p_version_id, p_document_id;
    END IF;

    -- Deprecate-first to satisfy non-deferrable partial unique index:
    UPDATE document_versions
    SET lifecycle_status = 'deprecated', superseded_at = COALESCE(superseded_at, v_now)
    WHERE company_id = p_company_id AND document_id = p_document_id
      AND lifecycle_status = 'active' AND id <> p_version_id;

    UPDATE document_versions
    SET lifecycle_status = 'active', activated_at = COALESCE(activated_at, v_now),
        superseded_at = NULL
    WHERE company_id = p_company_id AND document_id = p_document_id
      AND id = p_version_id;
END;
$$;
```

**V011** - `users.deactivated_at`, `memberships.expires_at`, chat_turns JSONB guards.

**V012** - `move_org_unit()` with session-flag GUC pattern (set_config) to
suppress the closure self-check trigger during the 3-step closure surgery.

**V013** - `reactivate_user()` canonical sign-in upsert.

**V015 - outbox_events**

```sql
CREATE TABLE outbox_events (
    id                 UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    company_id         UUID NOT NULL REFERENCES companies(id) ON DELETE CASCADE,
    aggregate_id       UUID,
    job_type           TEXT NOT NULL,
    job_schema_version SMALLINT NOT NULL DEFAULT 1,
    payload            JSONB NOT NULL,
    idempotency_key    TEXT NOT NULL,
    claimed_at         TIMESTAMPTZ,
    claimed_by         TEXT,        -- 'hostname:pid'
    completed_at       TIMESTAMPTZ,
    attempt_count      INT NOT NULL DEFAULT 0,
    last_error         TEXT,
    created_at         TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE (company_id, job_type, idempotency_key)
);
CREATE INDEX outbox_events_pending_idx ON outbox_events (company_id, created_at)
    WHERE completed_at IS NULL AND claimed_at IS NULL;
CREATE INDEX outbox_events_stuck_idx ON outbox_events (claimed_at)
    WHERE completed_at IS NULL AND claimed_at IS NOT NULL;
```

**V016** - `usage_events` (append-only, monthly partitions 2026-06 through 2027-06
plus default catch-all). Tokens and cost recorded per LLM call.

**V017** - `prompt_templates` (immutable content/hash columns enforced by trigger).
`chat_turns.prompt_template_id` added as nullable composite FK.

**V019** - `companies.region TEXT NOT NULL DEFAULT 'default'`.

**V020** - `companies.default_retention_days`, `documents.retention_until`,
`chat_turns.retention_until`. Sweep job is out of scope.

**V022** - Wiki lifecycle parity (see key patterns above).

**V024 - Temporal access history**

```sql
CREATE TABLE memberships_history (
    history_id  BIGSERIAL PRIMARY KEY,
    live_row_id UUID NOT NULL,
    company_id  UUID NOT NULL,
    user_id UUID, group_id UUID, org_unit_id UUID NOT NULL,
    role TEXT NOT NULL, applies_to TEXT NOT NULL,
    granted_by UUID, granted_at TIMESTAMPTZ NOT NULL, expires_at TIMESTAMPTZ,
    valid_from  TIMESTAMPTZ NOT NULL,
    valid_to    TIMESTAMPTZ,
    change_kind TEXT NOT NULL CHECK (change_kind IN ('insert','update','delete')),
    UNIQUE (live_row_id, valid_from)
);
CREATE UNIQUE INDEX memberships_history_open_uidx
    ON memberships_history (live_row_id) WHERE valid_to IS NULL;
-- Symmetric resource_grants_history table with same structure.

-- AFTER INSERT OR UPDATE OR DELETE trigger writes history:
CREATE OR REPLACE FUNCTION memberships_write_history()
RETURNS TRIGGER LANGUAGE plpgsql AS $$
DECLARE v_now TIMESTAMPTZ := clock_timestamp();
BEGIN
    IF TG_OP = 'INSERT' THEN
        INSERT INTO memberships_history (..., valid_from, valid_to, change_kind)
        VALUES (..., COALESCE(NEW.granted_at, v_now), NULL, 'insert');
        RETURN NEW;
    END IF;
    IF TG_OP = 'UPDATE' THEN
        UPDATE memberships_history SET valid_to = v_now
        WHERE live_row_id = OLD.id AND valid_to IS NULL;
        INSERT INTO memberships_history (..., valid_from, valid_to, change_kind)
        VALUES (..., v_now, NULL, 'update');
        RETURN NEW;
    END IF;
    IF TG_OP = 'DELETE' THEN
        UPDATE memberships_history SET valid_to = v_now
        WHERE live_row_id = OLD.id AND valid_to IS NULL;
        INSERT INTO memberships_history (..., valid_from, valid_to, change_kind)
        VALUES (..., v_now, v_now, 'delete');
        RETURN OLD;
    END IF;
END;
$$;
CREATE TRIGGER memberships_history_trigger
    AFTER INSERT OR UPDATE OR DELETE ON memberships
    FOR EACH ROW EXECUTE FUNCTION memberships_write_history();
```

**V025** - `wiki_pages.deleted_at`, `wiki_page_version_tombstones` (append-only,
content_hash proof, no FK on wiki_page_version_id, BEFORE UPDATE/DELETE trigger).

**V026** - AFTER DELETE triggers on `documents`, `wiki_pages`, `org_units` to
clean up orphaned `resource_grants` rows (polymorphic resource_id cannot use FK).
The `org_units` trigger covers both directions (as resource and as principal).

---

## Part 2 - C++ Iteration 1 code

### domain/types.hpp

```cpp
using Uuid = std::string;  // lower-case hyphenated

struct Error {
    enum class Kind { NotFound, Conflict, Forbidden, InvalidInput,
                      InvalidState, DatabaseError, ServiceUnavailable };
    Kind kind; std::string message;
    static Error not_found(std::string msg);
    static Error conflict(std::string msg);
    static Error forbidden(std::string msg);
    static Error invalid_input(std::string msg);
    static Error invalid_state(std::string msg);
    static Error database_error(std::string msg);
    static Error unavailable(std::string msg);
};

template<class T>
using Result = std::expected<T, Error>;

struct Principal {
    Uuid user_id; std::string email; std::string display_name;
    bool is_admin = false; bool is_service_account = false;
};

struct RequestContext {
    Tenant tenant; Principal principal; TraceSpan span;
    std::chrono::steady_clock::time_point deadline;
    bool deadline_exceeded() const {
        return std::chrono::steady_clock::now() >= deadline;
    }
};
```

### adapters/postgres/unit_of_work.hpp

```cpp
class UnitOfWork {
public:
    static drogon::Task<UnitOfWork> begin(drogon::orm::DbClientPtr db);

    template<typename... Args>
    drogon::Task<drogon::orm::Result>
    exec(std::string sql, Args&&... args) {
        co_return co_await tx_->execSqlCoro(std::move(sql), std::forward<Args>(args)...);
    }

    drogon::Task<void> commit();
    void rollback();
    ~UnitOfWork();  // logs warning + calls rollback() if not committed

private:
    TxPtr tx_;
    bool  committed_ = false;
};
```

### ingest/types.hpp

```cpp
struct ParsedSection {
    std::string heading; int depth = 0; std::string text;
    std::vector<ParsedSection> children;
    std::optional<std::string> db_id;  // populated after DB write
};

struct ParsedDocument {
    std::string filename; std::string mime_type;
    std::vector<ParsedSection> sections; std::string full_text;
};

struct Chunk {
    std::string document_version_id; std::string company_id;
    int chunk_index = 0; std::string text;
    std::optional<std::string> section_id;
    std::optional<std::string> section_heading;
    std::optional<std::string> db_id;  // populated after DB write
};

enum class IngestStatus { pending, processing, done, error };
```

### ingest/parser.hpp + parser.cpp (PlainTextParser)

`PlainTextParser::parse()` runs:

1. `resolve_text_mime()`:
   - Rejects empty or oversized content
   - EICAR detection: `content.find(kEicarPrefix) != npos` AND
     `content.find(kEicarMarker, eicar_prefix + prefix.size()) != npos`
   - Rejects PDF magic bytes (`%PDF-`) and ZIP magic bytes (`PK\x03\x04`)
   - Rejects null bytes (binary guard)
   - Determines MIME from extension or caller-provided mime_type
2. Splits into `ParsedSection` tree by ATX heading lines (`^#{1,6} `).
3. Returns `ParsedDocument` with top-level `sections` and `full_text`
   (concatenation of all body text).

### ingest/chunker.hpp + chunker.cpp

- `kMaxChars = 600`, `kOverlap = 100`
- Section-aware: chunks do not cross section boundaries
- `split_text()`: slides a window of `kMaxChars` chars, snaps end to the
  last whitespace within the window, carries `kOverlap` chars as prefix
  of the next chunk
- `chunk_index` is 0-based across the entire document

### ingest/document_repo.hpp (interface + PostgresDocumentRepo)

Key contracts:
- `fetch_access_scopes(company_id, document_id)`: returns the owner
  org_unit_id plus all org_units that have a `resource_grant` with
  `document_read` (or equivalent) on this document
- `write_sections(doc, version_id, company_id, uow)`: idempotent upsert
  into `document_sections`; populates `ParsedSection::db_id` in-place
- `write_chunks(chunks, company_id, access_scope_ids, uow)`: idempotent
  upsert into `document_chunks`; ON CONFLICT on `(document_version_id, chunk_index)`
- `set_ingest_status(...)`: updates `document_versions.ingest_status` to
  `pending`, `processing`, or `error`; rejects `done` with an error
- `mark_ingest_done(...)`: sets `ingest_status='done'`, `completed_at`,
  `chunk_count` inside the provided UoW

### rag/types.hpp (key excerpts)

```cpp
// UUID v5 for deterministic Qdrant point IDs.
// Point ID = uuid_v5(chunk_id + ":" + embed_model_id)
// On EVP_MD_CTX_new() failure: returns "00000000-0000-0000-0000-000000000000"
inline std::string uuid_v5(std::string_view name) { /* EVP SHA-1 */ }

struct ChunkPayload {
    static constexpr int kSchemaVersion = 1;
    std::string company_id, document_id, document_version_id, chunk_id;
    int chunk_index = 0;
    std::vector<std::string> access_scope_ids;
    std::string sensitivity_label = "internal";
    std::string lifecycle_status  = "draft";
    std::optional<std::string> activated_at, superseded_at;
    std::optional<std::string> section_id, section_heading;
    int payload_schema_version = kSchemaVersion;
};

struct ChunkCandidate { std::string chunk_id, document_version_id; float score; ChunkPayload payload; };
struct AllowedCandidate { std::string chunk_id, document_version_id; float score; std::string text; std::optional<std::string> section_heading; };
struct QdrantFilter { std::string company_id; std::vector<std::string> access_scope_ids; std::string lifecycle_status = "active"; };
struct UpsertPoint { std::string id; Embedding vector; ChunkPayload payload; };
```

### rag/embedder.hpp + embedder.cpp (LlamaEmbedder)

Single persistent `drogon::HttpClientPtr` (keep-alive, pipelining depth 4).
POST to `/v1/embeddings` with `{"model":"...","input":[texts]}`.
Parses `data[0..n].embedding` from OpenAI-compatible response.
Returns error if `parsed.data.size() != texts.size()`.
Does NOT sort `data` by `index` field before returning.

### rag/vector_store.hpp (QdrantVectorStore interface)

```cpp
virtual drogon::Task<Result<void>> ensure_collection(int dims) = 0;
virtual drogon::Task<Result<void>> upsert(const std::vector<UpsertPoint>& points) = 0;
virtual drogon::Task<Result<void>> delete_by_version(std::string_view company_id,
                                                      std::string_view document_version_id) = 0;
virtual drogon::Task<Result<std::vector<ChunkCandidate>>>
search(const Embedding& query, const QdrantFilter& filter, int limit = 20) = 0;
```

### application/ingest_document_version.hpp + .cpp

Constructor: `(db, repo, parser, chunker)` - no embedder; embedding is deferred
to the outbox worker that drains `qdrant_upsert_chunk_payload` events.

`execute(ctx, cmd)` steps:
1. Check deadline; check `ctx.tenant.company_id == cmd.company_id`
2. `set_ingest_status -> processing` (outside UoW; survives rollback)
3. Read file from `cmd.file_path` using `std::filesystem::file_size` + `std::ifstream`
4. `parser_->parse(content, cmd.file_path, {})` - MIME passed as empty
5. `fetch_access_scopes(company_id, document_id)`
6. `UnitOfWork::begin(db_)`
7. `write_sections(doc, version_id, company_id, uow)` - fills `db_id`s
8. `chunker_.chunk(doc, version_id, company_id)` - uses `db_id`s set above
9. `write_chunks(chunks, company_id, access_scope_ids, uow)`
10. INSERT `audit_log` row inside UoW (action: `doc.ingest.chunks_written`)
11. INSERT `outbox_events` row inside UoW (job_type: `qdrant_upsert_chunk_payload`,
    idempotency_key: `qdrant_upsert:{version_id}:{embed_model_id}:{trace_id}`)
12. `mark_ingest_done(company_id, version_id, chunk_count, uow)`
13. `uow.commit()`

Error path: `co_await fail(e)` - `fail` is a `[&]`-capturing lambda coroutine
that calls `set_ingest_status -> error` then returns `std::unexpected(e)`.

### application/promote_document_version.hpp + .cpp

```cpp
// execute steps:
// 1. deadline check
// 2. UoW begin
// 3. SELECT promote_document_version($1, $2, $3)  -- SQL function (V010)
// 4. INSERT audit_log (action: 'document.version.promoted')
// 5. INSERT outbox_events (job_type: 'qdrant_resync_version_lifecycle',
//    idempotency_key: promote:{doc_id}:{version_id}:{trace_id})
// 6. uow.commit()
```

### auth.hpp (declarations only; implementation in auth.cpp)

```cpp
void auth_init(const Config& cfg);  // load OIDC JWKS synchronously at startup

struct Identity {
    std::string user_id; std::string email;
    std::string display_name; bool is_admin = false;
};

std::optional<Identity> validate_jwt(std::string_view token);
std::optional<Identity> validate_api_key(std::string_view key);  // no company_id param
std::optional<Identity> authenticate(const drogon::HttpRequestPtr& req);
```

### access.hpp (AccessService declarations)

```cpp
drogon::Task<std::vector<std::string>>
effective_read_orgs(std::string_view company_id,
                    std::string_view user_id,
                    std::string_view org_unit_id);

drogon::Task<bool>
has_role(std::string_view user_id, std::string_view org_unit_id, Role required);

drogon::Task<void>
add_member(company_id, org_unit_id, principal_type, principal_id, role,
           self_and_descendants, granted_by);
drogon::Task<void> remove_member(...);
drogon::Task<void> change_role(...);
drogon::Task<void> grant_resource(company_id, resource_type, resource_id,
                                   principal_type, principal_id, permission,
                                   self_and_descendants, granted_by);
drogon::Task<void> revoke_resource(...);

private:
drogon::Task<void> invalidate_cache(company_id, user_id, org_unit_id);
```

Redis cache key: `lr:eff:{company_id}:{user_id}:{org_unit_id}`.

---

## Questions

Please review the schema (V001-V028) and the Iteration 1 C++ code above for:

1. **Schema correctness**: are there any constraint gaps, lifecycle invariant
   holes, or FK coverage problems that could let bad data into the DB?

2. **Trigger correctness**: are any of the triggers (history, immutability,
   append-only, actor validation, resource_grants cleanup, lifecycle timestamps)
   logically incorrect or vulnerable to race conditions?

3. **C++ correctness**: are there bugs in the ingest use case, chunker, embedder,
   or type design that could cause data corruption, silent failures, or
   security violations?

4. **Access control gaps**: is there any path where a principal could read
   data they should not have access to, given the schema design and
   `AccessService` as declared?

5. **Outbox and idempotency**: are the outbox event idempotency keys correct?
   Is there any case where re-running a failed ingest or promote produces
   a duplicate or missing side-effect?

6. **Anything else** that stands out as a likely production bug or a design
   decision that will cause pain when the HTTP routes are implemented in
   iteration 2.

Focus on real issues. If something is correctly designed, say so in one
sentence and move on.
