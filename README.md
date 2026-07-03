# Wikore

Wikore is a self-hosted, permission-aware enterprise wiki and
retrieval-augmented generation (RAG) backend. It is designed for organizations
that need document retrieval to respect tenant, organizational, lifecycle, and
sensitivity boundaries.

The project is written in C++23 with Drogon. PostgreSQL is the authoritative
store, Qdrant is a rebuildable vector index, and Redis provides queues and
short-lived coordination.

> **Project status:** active development, not production-ready. The Iteration 1
> ingestion, transactional outbox, vector-indexing worker, and crash-recovery
> foundation are implemented. Iteration 2 adds the access-resolution read path -
> the snapshot-consistent reader-scope resolver with its epoch-validated Redis
> cache, sensitivity clearance, the Qdrant prefilter builder, the EvidenceGate
> authorization boundary, and the retrieval orchestrator - implemented and tested
> at the library level (see [`docs/iteration_2_design.md`](docs/iteration_2_design.md)).
> The scheduler also refreshes stale ACL payloads in every affected Qdrant
> collection without re-embedding. Wiring retrieval into HTTP routes, as-of and
> section-expanded retrieval, non-text parsers, the chat pipeline, wiki features,
> and the evaluation CLI are still under development.

## Architecture

```mermaid
flowchart LR
    Client[API clients] --> API[wikore_api]
    API --> PG[(PostgreSQL<br/>authoritative state)]
    API -. planned upload enqueue .-> Queue[(Redis<br/>tenant ingest queues)]

    Queue --> Ingest[wikore_ingest]
    Ingest -->|chunks + audit + outbox<br/>one transaction| PG

    PG -->|ingest + ACL-resync<br/>outbox events| Scheduler[wikore_scheduler]
    Scheduler --> Embed[OpenAI-compatible<br/>embedding endpoint]
    Scheduler --> Qdrant[(Qdrant<br/>derived vector index)]
    Scheduler -->|vector bookkeeping| PG

    Scheduler <-->|heartbeats, recovery queues| Queue

    API -. planned retrieval .-> Embed
    API -. filtered candidates .-> Qdrant
    API -. evidence hydration .-> PG
    API -. grounded prompt .-> LLM[OpenAI-compatible<br/>LLM endpoint]
```

Embedding and Qdrant writes do not occur inside the ingest transaction. The
ingest worker commits the durable document state and an outbox event first. The
scheduler later embeds the committed chunks, upserts deterministic Qdrant
points, and records the chunk-to-vector mapping in PostgreSQL. ACL changes
enqueue separate resync events; the scheduler recomputes visibility from live
PostgreSQL state and patches the affected payloads in every Qdrant collection
without re-embedding.

### Components

| Binary | Responsibility | Current status |
| --- | --- | --- |
| `wikore_api` | Authenticated document, organization, wiki, chat, and administration API | Health route works; most application routes return `501` |
| `wikore_ingest` | Fair per-tenant queue consumption, parsing, chunking, and transactional persistence | Implemented for plain text and Markdown |
| `wikore_scheduler` | Ingest and ACL-resync outbox draining, embedding, multi-collection Qdrant payload refresh, ingest recovery sweeps, and partition maintenance | Iteration 1 and ACL-resync paths implemented |
| `wikore_eval` | Retrieval and answer-quality evaluation harness | Startup stub; planned for a later iteration |

The source is organized as a modular monolith, while expensive and scheduled
work runs in separate processes. The core libraries can also be exercised with
in-memory test adapters.

## Design Invariants

- **PostgreSQL is authoritative.** Chunk content, lifecycle state, audit data,
  outbox events, and vector bookkeeping live in PostgreSQL. Redis and Qdrant are
  treated as derived or transient stores.
- **Tenant boundaries are database-enforced.** Cross-entity relationships use
  tenant-scoped composite foreign keys where possible, rather than relying only
  on application checks.
- **External side effects are asynchronous.** The ingest use case writes chunks,
  audit data, terminal ingest state, and the outbox event in one transaction.
- **Retries are idempotent.** Chunk rows use stable conflict keys and Qdrant
  points use deterministic UUIDs derived from the chunk and embedding model.
- **Worker ownership is explicit.** Ingest claims use per-claim tokens so a
  stale worker cannot overwrite a newer worker's terminal result.
- **Crashes remain recoverable.** Redis processing lists, worker heartbeats,
  persisted ingest payloads, bounded retry counters, stale outbox claims, and
  polling sweeps cover the worker-crash windows implemented in Iteration 1.
- **Retrieval must fail closed.** The prefilter and the EvidenceGate both return
  no candidates when the resolved access scope or the sensitivity clearance is
  empty.
- **Postgres is the evidence (gate authority).** A retrieved chunk reaches an
  answer only after the EvidenceGate re-validates it against live PostgreSQL -
  tenant, lifecycle, sensitivity, and resource visibility resolved from
  `resource_grants` and `org_unit_closure`. The Qdrant payload is a recall
  prefilter, never the access authority, so a stale index can only drop
  candidates, never leak one. The scheduler asynchronously resynchronizes stale
  ACL payloads; as-of retrieval is still under development.

## Ingestion Flow

1. A document version is persisted as `pending` and its job is placed on a
   tenant-specific Redis queue.
2. `wikore_ingest` rotates fairly across discovered tenant queues and moves one
   job to a worker-specific processing list.
3. A compare-and-set transition claims the version as `processing`, persists
   the recovery payload, and creates an ownership token.
4. The worker validates and parses the file, builds a section tree, creates
   section-aware overlapping chunks, and resolves the chunk access snapshot.
5. Sections, chunks, an audit row, the outbox event, and `ingest_status='done'`
   commit atomically in PostgreSQL.
6. `wikore_scheduler` claims the outbox event with `FOR UPDATE SKIP LOCKED`,
   validates the configured embedding model against the model registry, embeds
   the chunks in batches, and upserts them into the model's Qdrant collection.
7. The scheduler records `document_chunk_vectors` rows and marks the outbox
   event complete. Transient failures use persisted exponential backoff.

On shutdown, all four scheduler workers drain in-flight work before exiting.
Long polling and partition-maintenance sleeps wake in bounded chunks so
SIGTERM/SIGINT is observed promptly. If an ingest worker dies, the scheduler can
recover its processing list or requeue a stale database claim. Poison jobs have
a bounded resume budget and eventually transition to `error` rather than
looping forever.

## Access Resolution and Retrieval (Iteration 2)

The read path is permission-aware and resolves access against the authoritative
tree at query time rather than trusting denormalized index payloads:

1. `AccessResolver` resolves a principal's read scope (the org_units they may
   read) in one snapshot-consistent statement, and stamps it with the
   `companies.acl_epoch` and `users.scope_epoch` it was computed under.
2. `CachedAccessResolver` serves the scope from Redis (`lr:eff:*`), validated
   against those live epochs so a stale entry is detectable even if its
   invalidation was lost; entries also carry a TTL clamped to membership expiry.
3. `QdrantFilterBuilder` turns the scope plus the derived sensitivity clearance
   into the vector prefilter (tenant, access scope, sensitivity labels, active
   lifecycle). Clearance derivation is a single enforced point that is
   fail-closed on the top tier.
4. The `EvidenceGate` re-validates each retrieved candidate against live
   PostgreSQL - resolving resource visibility from `resource_grants` and
   `org_unit_closure`, not the index payload - and hydrates the survivors. Only
   the gate can produce an `AllowedCandidate`, so nothing reaches the reranker
   without passing it.
5. `RetrievalOrchestrator` composes the above: embed, resolve, derive clearance,
   prefilter, search, gate.

Two invariants govern the layer, both covered by a randomized property test that
drives the real gate against an independent oracle:

- **G1 (gate authority):** the gate, resolving live from PostgreSQL, is the only
  thing that authorizes a chunk; the prefilter is advisory.
- **G2 (same-transaction epochs):** every `acl_epoch` / `scope_epoch` /
  `acl_version` bump commits in the same transaction as the change it describes
  (enforced by V032 triggers), so observing a new epoch implies the change is
  already visible.

The full design note is [`docs/iteration_2_design.md`](docs/iteration_2_design.md).

## Build

Wikore currently targets Linux. CI builds on Ubuntu 24.04 with GCC and CMake
3.28 or newer.

Install the system dependencies used by CI:

```sh
sudo apt-get update
sudo apt-get install cmake ninja-build ccache libspdlog-dev libssl-dev \
  libhiredis-dev libjsoncpp-dev libpq-dev uuid-dev git
```

Configure and build:

```sh
cmake -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DWIKORE_NATIVE_OPTS=OFF
cmake --build build --parallel
```

CMake fetches Drogon, Glaze, jwt-cpp, and Catch2 during configuration. Debug
builds enable AddressSanitizer, UndefinedBehaviorSanitizer, and warnings as
errors.

Useful CMake options:

| Option | Default | Purpose |
| --- | --- | --- |
| `WIKORE_BUILD_APPS` | `ON` | Build all four executables |
| `WIKORE_BUILD_TESTS` | `ON` | Build the Catch2 test executable |
| `WIKORE_NATIVE_OPTS` | `ON` | Enable native CPU and link-time optimizations in Release builds |

## Database Setup

Wikore requires PostgreSQL 17. It does not yet include an application-level
migration runner. Provision the NOLOGIN partition-maintenance grant role before
applying migrations:

```sh
# 1. Provision the grant role (run as CREATEROLE; idempotent, no credentials)
psql -h localhost -U postgres -d wikore \
  -v ON_ERROR_STOP=1 -f db/provision_roles.sql

# 2. Apply schema migrations
export DATABASE_URL=postgresql://wikore:wikore@localhost:5432/wikore
ls db/migrations/V*.sql | sort | xargs cat \
  | psql "$DATABASE_URL" -v ON_ERROR_STOP=1
```

Create a separate non-superuser login through your normal secret-management or
DBA workflow and grant it only the partition-maintenance role:

```sql
GRANT wikore_partition_maintainer TO your_partition_login;
```

The repository intentionally does not create that login or store a default
password. Configure the scheduler with its connection URL:

```sh
export PARTITION_DATABASE_URL=postgresql://your_partition_login:${PASSWORD}@localhost:5432/wikore
```

The scheduler also requires an enabled embedding-model registry row whose name
and dimension match `EMBED_MODEL` and `EMBED_DIMS`:

```sql
INSERT INTO embedding_models (name, qdrant_collection, dimension)
VALUES ('bge-m3', 'wikore_docs_bge_m3', 1024);
```

Use the values exposed by your embedding server. The scheduler fails fast with
a non-zero exit status if the model is missing or disabled, dimensions differ,
or the Qdrant collection cannot be initialized.

For the example above, export matching runtime values before starting workers:

```sh
export EMBED_MODEL=bge-m3
export EMBED_DIMS=1024
```

## Configuration

Configuration is read from environment variables. The checked-in
[`.env.example`](.env.example) is a starting point.

| Variable | Default | Used for |
| --- | --- | --- |
| `DATABASE_URL` | `postgresql://wikore:wikore@localhost:5432/wikore` | PostgreSQL connection |
| `PARTITION_DATABASE_URL` | required by scheduler | Restricted partition-maintenance connection |
| `REDIS_URL` | `redis://127.0.0.1:6379/0` | Queues, heartbeats, and caches |
| `QDRANT_URL` | `http://localhost:6333` | Vector index |
| `EMBED_BASE_URL` | `http://localhost:8081/v1` | OpenAI-compatible embeddings endpoint |
| `EMBED_MODEL` | empty | Registry model name used by ingest and scheduler |
| `EMBED_DIMS` | `768` | Expected embedding dimension |
| `LLM_BASE_URL` | `http://localhost:8080/v1` | OpenAI-compatible generation endpoint |
| `LLM_MODEL` | empty | Generation model name |
| `LLM_MAX_TOKENS` | `2048` | Maximum generated tokens |
| `LLM_CONCURRENCY` | `4` | Planned global in-flight generation cap |
| `OIDC_ISSUER` | empty | OIDC issuer used to load and validate JWKS |
| `OIDC_AUDIENCE` | `wikore` | Expected token audience |
| `CREDENTIALS_KEY` | empty | 64-character hex AES-256-GCM key for integration secrets |
| `PORT` | `9000` | API listen port |
| `ALLOWED_ORIGIN` | `*` | Planned API CORS origin setting; wiring is incomplete |
| `ANTHROPIC_API_KEY` | empty | Optional future wiki-generation provider key |
| `ANTHROPIC_MODEL` | `claude-sonnet-4-6` | Optional future wiki-generation model |

Generate a credential-encryption key with:

```sh
openssl rand -hex 32
```

## Running Locally

After PostgreSQL, Redis, Qdrant, and the embedding endpoint are available and
the schema/model registry are initialized, start the background processes:

```sh
./build/apps/scheduler/wikore_scheduler
./build/apps/ingest/wikore_ingest
```

The API can be started separately:

```sh
./build/apps/api/wikore_api
curl http://localhost:9000/api/health
```

At the current stage, the health route is the only completed public HTTP flow;
ingest behavior is primarily exercised through integration tests and internal
use cases.

## Testing

```sh
# C++ unit tests; DB-backed cases skip when DATABASE_URL is absent
ctest --test-dir build --output-on-failure

# Load every migration into PostgreSQL 17 and verify schema behavior
make smoke

# Run PostgreSQL + Redis integration tests using local service containers
bash scripts/test_integration_local.sh

# Verify deterministic corpus tooling and generated negative samples
make corpus-test corpus-verify
```

CI runs three required jobs on every pull request:

1. PostgreSQL 17 schema smoke tests.
2. Release build and non-database Catch2 tests.
3. PostgreSQL and Redis integration tests using the compiled test artifact.

## Current Scope

Implemented foundation:

- PostgreSQL migrations through V032, including tenant constraints, lifecycle
  state, append-only records, temporal access history, outbox retries, and ingest
  ownership tokens.
- Hardened UTF-8 plain-text and Markdown parsing with size, MIME, binary,
  Unicode-tag, hidden-HTML, and EICAR checks.
- Section-aware chunking and transactional document persistence.
- Fair multi-tenant ingest consumption with graceful shutdown and crash recovery.
- Embedding-model registry validation, batched embeddings, deterministic Qdrant
  writes, and PostgreSQL vector bookkeeping.
- OIDC/JWT and API-key authentication foundations, Qdrant adapters, null test
  adapters, schema smoke tests, and integration coverage.
- Iteration 2 access resolution: the snapshot-consistent reader-scope resolver
  with an epoch-validated Redis cache, group-aware resolution, sensitivity
  clearance derivation, the Qdrant prefilter builder, the EvidenceGate live
  authorization boundary, and the retrieval orchestrator. Covered by a
  randomized gate property test (validated against an independent oracle) and
  integration tests.
- V031-V032 migrations: runtime partition maintenance, and the ACL epoch,
  per-document version, and per-user scope-epoch columns with same-transaction
  bump triggers.
- Qdrant ACL resynchronization: the scheduler serializes ingest and resync per
  document, recomputes visibility from PostgreSQL, and refreshes schema-v3
  payloads across every indexed embedding-model collection without
  re-embedding.

Not yet complete:

- Production document upload and administration routes, and HTTP retrieval
  routes wiring the orchestrator to a live embedder and Qdrant.
- As-of and section-expanded retrieval, and execution of the one-time schema-v3
  payload backfill for pre-existing points.
- PDF, DOCX, HTML, and other rich-document parsers.
- Reranking, answer generation, and SSE streaming.
- Wiki operations, MCP integrations, retention jobs, and the evaluation harness.
