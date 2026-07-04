# BaryGraph Lite for Wikore

## Status

This document is a design proposal for discussion. It is not an implementation specification that has already been accepted, and none of the new modes, tables, ports, or endpoints described here should be assumed to exist yet.

## Review summary (2026-07-04, Opus verification pass)

> **[REVIEW]** Verdict: factually accurate, architecturally sound, and
> implementable in Wikore. Three concrete DDL defects (inline markers
> REVIEW-D1..D3) must be fixed before any migration is written, and the
> scoping recommendation deserves pushback (inline markers REVIEW-S1..S3).
>
> **Codebase claims verified exact** (checked 2026-07-04 against `main`):
> `documents.authority_level` NOT NULL DEFAULT 50 CHECK 0-100 (V003:100);
> `embedding_models` at V003:256; `memberships_history` pattern (V024);
> orphan-chunk hard delete at `document_repo.cpp:404`;
> `document_sections.parent_section_id` composite self-FK (V014:115);
> `wiki_page_sources` (V004); append-only `audit_log` trigger and
> unconstrained `action` (V007:85); pg_constraint introspection test at
> `test_error_mapper.cpp:162` (a V034 migration WILL fail CI without the
> error-mapper updates this doc lists); `UNIQUE(company_id, id)` composite-FK
> targets on users/org_units/document_chunks/groups; `resource_grants`
> principal restriction to org units; absence of ContextBuilder/AskQuestion/
> AnswerFinalizer; V034 as the next free migration number.
>
> **External citations verified** against the BaryGraph Kaikki PoC v0.6
> (fetched 2026-07-04): the L14/L15 formula, per-type q_seed values
> (contradicts=0.85, synonyms=0.90), the `s06_l14_edges.py` type-vector
> stage, the removed LLM summary vector, and the 15-level MetaBary/MongoDB
> design are all represented accurately. The q=1-eliminates-the-type-vector
> observation and the floor=10 arithmetic (minimum endpoint share ~9%) are
> mathematically correct.
>
> **Recommendation:**
> 1. During Iteration 3, adopt ONLY step 1 of the implementation sequence:
>    ContextBuilder accepting `span<const AllowedEvidence>` with a chunk-only
>    variant member for now. Do not block the iter-3 chat endpoint on any
>    other part of this design (see REVIEW-S1).
> 2. Ship deterministic impact analysis over the NATIVE tables first
>    (wiki_page_sources, sections, superseded versions, chat_turns.rag_sources):
>    no new schema, immediate governance value (see REVIEW-S3).
> 3. Split V034: edge core only; privileged-access/break-glass ships
>    separately and only when a customer requires it (see REVIEW-S2).
> 4. Defer the Qdrant edge collection until an edge source exists that is not
>    purely manual (Phase 2 parser extraction): admin-only edges give the
>    vector search almost nothing to search.
> 5. Fix REVIEW-D1..D3 before writing the migration, and answer open
>    question #5 (first customer-visible value) before starting.

The working name in Wikore is **BaryGraph Lite**. The external inspiration is the [BaryGraph Kaikki proof of concept](https://github.com/oleksiy-perepelytsya/bary-vector/blob/master/BaryGraph_Kaikki_PoC_v0_6.md), but Wikore is not attempting to reproduce its full hierarchy, MetaBary recursion, MongoDB implementation, or corpus-specific algorithms.

The implementation must remain C++23 within the existing Wikore application. PostgreSQL remains authoritative storage, Qdrant remains a recall-oriented vector index, Redis remains appropriate for bounded coordination and caching, and Drogon remains the API/runtime framework. SQL migrations and set-based authorization queries are expected; a separate Python graph pipeline or graph service is not.

## Executive summary

Wikore currently retrieves chunks. BaryGraph Lite adds independently retrievable **relationship evidence** connecting chunks, sections, document versions, wiki pages, teams, and eventually other governed knowledge objects.

BaryGraph Lite is an optional analysis capability, not Wikore's primary product function. Wikore remains a permission-safe knowledge-base wiki and chat system in which both surfaces use RAG. Ordinary employees should continue to receive fact-oriented wiki and chat retrieval without gaining relationship discovery, cross-department traversal, impact analysis, or retrieval diagnostics automatically.

Relationship analysis is intended for explicitly selected roles or groups, such as designated executives, directors, governance teams, consultants, legal reviewers, compliance personnel, investigators, and other approved specialists. Job title alone must not grant it. Tenant administrators must assign a specific capability, and that capability must remain independent from access to the underlying evidence.

Examples:

- A policy chunk says contractors require approval.
- A Finance chunk identifies the team responsible for approving contractor budget.
- A relationship says the Finance budget rule implements or satisfies the approval dependency in the contractor policy.

Flat vector retrieval may find either chunk. Relationship retrieval can find the governed connection between them and provide both endpoints as evidence.

The critical architectural requirement is not merely a new edge table or vector formula. It is a generalized evidence authorization boundary:

```text
raw chunk candidate          -> EvidenceGate -> AllowedChunk
raw relationship candidate   -> EvidenceGate -> AllowedRelationship
raw path candidate           -> EvidenceGate -> AllowedPath

AllowedEvidence only -> reranking -> ContextBuilder -> LLM
```

A relationship is allowed only when every endpoint required to explain it is independently visible to the caller under live PostgreSQL state. The existence, type, direction, confidence, and endpoint identifiers of a rejected relationship must also be treated as potentially sensitive.

BaryGraph Lite should be introduced before Iteration 3 is closed because `ContextBuilder`, `AskQuestion`, `AnswerFinalizer`, and the production chat endpoint are not yet complete. Building those components around `AllowedEvidence` now avoids hard-coding the assumption that all evidence is an isolated chunk.

> **[REVIEW-S1]** Partial disagreement on timing. The only cheap insurance
> Iteration 3 needs is step 1 of the implementation sequence: `ContextBuilder`
> taking `span<const AllowedEvidence>` with a chunk-only variant member for
> now. This doc itself concedes (in the domain-model section) that retaining
> `AllowedCandidate` and introducing the variant later is the pragmatic first
> step. Blocking the iter-3 chat endpoint on edge schema, indexing workers,
> and a second gate would delay the primary deliverable by weeks for a
> feature whose V1 has no automated edge source (see REVIEW-S3).

The resulting product model is:

```text
Wikore core
  -> permission-safe knowledge-base wiki
  -> permission-safe RAG chat

Optional BaryGraph Lite capability
  -> relationship discovery
  -> bridge questions
  -> bounded path analysis
  -> impact and contradiction analysis

Restricted operational capabilities
  -> shadow diagnostics
  -> relationship administration/review
  -> temporary break-glass scope expansion
```

Enabling BaryGraph Lite does not expand a person's document permissions. It only permits relationship-oriented retrieval over evidence they can already access. A separate, explicitly approved break-glass session may temporarily expand evidence scope, but it is not the normal mechanism for using relationship analysis.

## Why relationship-aware retrieval matters

### Relationship-aware RAG

Enterprise answers often depend on the relationship between sources rather than facts contained in one source.

Representative relationships include:

- policy -> exception;
- policy -> implementing procedure;
- SOP -> responsible team;
- runbook -> system dependency;
- contract -> renewal clause;
- security rule -> affected team;
- wiki page -> source document version;
- requirement -> control implementation;
- procedure -> approval dependency.

Questions such as “Who needs to approve contractor onboarding?” may require joining an HR policy with a Finance responsibility rather than retrieving a single semantically similar paragraph.

### Cross-department bridge discovery

Cross-department processes are typically fragmented:

```text
HR onboarding policy
  -> IT account provisioning runbook
  -> Finance payroll activation procedure
```

Relationship retrieval can support questions such as:

- What systems and teams are involved when a new employee starts?
- Which Finance process is affected by this HR policy change?
- How does this security control affect onboarding?
- Why does Finance care about this HR procedure?

These are bridge questions. They ask for a defensible connection, not merely a nearest chunk.

### Impact analysis and knowledge governance

Relationships can also be traversed without invoking an LLM:

```text
changed document version
  -> affected chunks
  -> wiki pages citing those chunks
  -> related policies and procedures
  -> previous chat answers that cited those versions
```

This enables governance features such as:

- “Changing this policy affects four wiki pages and three procedures.”
- “Twelve historical answers cite the superseded version.”
- “These active pages still depend on a deprecated source.”
- “This control change reaches Finance through two documented dependencies.”

PostgreSQL traversal should be used for deterministic impact analysis. Vector search is useful for discovering likely relationships, but it is not required to follow already-governed edges.

### Contradiction, exception, and implementation discovery

Useful edge types include:

- `supersedes`;
- `contradicts`;
- `exception_to`;
- `implements`;
- `depends_on`;
- `same_requirement_as`;
- `derived_from`;
- `cites`;
- `responsible_team`;
- `affects`;
- `requires_approval_from`.

This can support questions such as:

- Are active wiki pages still citing deprecated policies?
- Does this SOP contradict a current policy?
- Which procedure implements this compliance requirement?
- Which exceptions apply to this rule?

### Meaningful section expansion

Section expansion should eventually use explicit relationships instead of only adjacent chunk positions:

- section `parent_of` subsection;
- section `contains` chunk;
- chunk `defines` term;
- chunk `states_exception_to` policy chunk;
- chunk `elaborates` preceding requirement;
- chunk `same_procedure_step_as` another chunk.

This makes expansion semantic and governed rather than a blind parent/sibling operation.

## What is borrowed from BaryGraph

The external BaryGraph proof of concept promotes relationships to first-class retrievable documents. Its atomic relationship vector is conceptually:

```text
bary_vec = normalize(q * left_vector
                   + q * right_vector
                   + (1 - q) * relation_type_vector)
```

Here, `q` represents connection quality and the type vector represents the semantics of the relationship itself. The proof of concept subsequently builds multi-level MetaBary structures recursively from lower-level relationship objects.

BaryGraph Lite borrows:

- relationships as first-class stored and retrievable objects;
- an independently indexed relationship vector;
- explicit relationship types;
- confidence or quality as a stored signal;
- structural bridge discovery across otherwise distant semantic neighborhoods;
- traceability from retrieved relationship back to its endpoints.

BaryGraph Lite does not initially borrow:

- the 15-level hierarchy;
- recursive MetaBary construction;
- unique-parent forest construction;
- orphan re-entry;
- corpus-specific matching thresholds;
- MongoDB or `mongot`;
- automated LLM summaries for every relationship;
- unbounded graph traversal;
- a separate graph database.

The BaryGraph document itself reports that an LLM-generated relationship-summary vector was removed because the algebraic edge vector was sufficient for the behavior under study. That supports a deterministic and embedding-first Wikore implementation rather than requiring an LLM extraction pipeline on day one.

## Terminology

### Knowledge object

Any governed object that can be an endpoint of a relationship. The first implementation should restrict endpoints to document chunks. Future endpoint kinds may include sections, document versions, wiki page versions, org units, systems, controls, and chat answers.

### Relationship or knowledge edge

A typed, directed or symmetric connection between two or more knowledge objects. The first implementation should require exactly two chunk endpoints.

### Edge endpoint

One object participating in an edge, with an ordinal and semantic role such as `subject`, `object`, `source`, `target`, `requirement`, or `implementation`.

### Edge candidate

A lightweight result returned by Qdrant before authoritative authorization. It must not contain hydrated source text.

### Allowed relationship

A relationship whose edge record and every required endpoint passed live authorization. It may contain hydrated endpoint evidence suitable for reranking and context construction.

### Allowed path

A bounded ordered sequence of allowed relationships where every distinct endpoint is visible. Paths are derived query results, not necessarily a separately persisted object.

### Fact retrieval

Normal chunk retrieval for questions likely answerable from individual sources.

### Bridge retrieval

Relationship and bounded-path retrieval for questions asking how concepts, teams, policies, systems, or procedures connect.

### Retrieval intent

A non-security choice controlling which candidate sources are searched. Retrieval intent must never alter tenant or authorization policy.

### Shadow diagnostics

A safe production diagnostic that records or returns metadata about why candidates were rejected without using rejected evidence for answer generation.

### Oracle evaluation

A fixture-only evaluation mode that compares retrieval behavior against known test truth. It must not be reachable in production.

### Break-glass

A separately authorized, temporary production scope expansion. It remains subject to tenant, evidence, lifecycle, tombstone, and audit invariants. It is not required to implement BaryGraph Lite and must not be confused with bridge retrieval.

## Product capability and entitlement model

Relationship retrieval requires two independent decisions:

1. May this principal use the relationship-analysis feature?
2. May this principal see every evidence endpoint in this particular relationship?

The first is a product/capability entitlement. The second is normal Wikore data authorization. Both must pass.

```text
tenant feature enabled
  AND principal capability granted
  AND requested operation allowed by that capability
  AND every evidence endpoint authorized
  -> relationship may be returned
```

These layers must not be collapsed into `is_admin`.

### Suggested capabilities

```cpp
enum class Capability {
    relationship_search,
    relationship_path_analysis,
    relationship_impact_analysis,
    relationship_review,
    relationship_admin,
    retrieval_diagnostics,
    break_glass_request,
    break_glass_approve,
    break_glass_use,
};
```

`oracle_eval` should not be a production capability. It belongs to a fixture-only test executable or build configuration.

Capabilities should be granular. A lawyer may need relationship search and impact analysis without relationship administration. A support investigator may need diagnostics metadata without permission to approve LLM-proposed relationships. A director may use bridge search without being allowed to activate break-glass.

### Assignment subjects

Entitlements may be assigned to:

- an individual user;
- an identity-provider group;
- a Wikore org unit, if inheritance semantics are explicitly defined;
- a temporary engagement group for consultants or external legal reviewers.

Do not infer entitlement from `is_admin`, email domain, display title, or C-level naming conventions. Use explicit assignment and audit it.

The existing `resource_grants` model is not sufficient for this purpose. It grants access to governed resources and currently restricts principals to org units. Feature entitlement should have a separate schema rather than overloading document/resource permissions.

### Suggested schema

Names are illustrative:

```sql
CREATE TABLE tenant_features (
    company_id   UUID NOT NULL,
    feature      TEXT NOT NULL,
    enabled      BOOLEAN NOT NULL DEFAULT false,
    enabled_by   UUID,
    enabled_at   TIMESTAMPTZ,
    PRIMARY KEY (company_id, feature)
);

CREATE TABLE user_capability_grants (
    company_id   UUID NOT NULL,
    user_id      UUID NOT NULL,
    capability   TEXT NOT NULL,
    granted_by   UUID NOT NULL,
    reason       TEXT NOT NULL,
    granted_at   TIMESTAMPTZ NOT NULL DEFAULT now(),
    expires_at   TIMESTAMPTZ,
    revoked_at   TIMESTAMPTZ,
    PRIMARY KEY (company_id, user_id, capability),
    FOREIGN KEY (company_id, user_id)
        REFERENCES users(company_id, id)
);

CREATE TABLE group_capability_grants (
    company_id   UUID NOT NULL,
    group_id     UUID NOT NULL,
    capability   TEXT NOT NULL,
    granted_by   UUID NOT NULL,
    reason       TEXT NOT NULL,
    granted_at   TIMESTAMPTZ NOT NULL DEFAULT now(),
    expires_at   TIMESTAMPTZ,
    revoked_at   TIMESTAMPTZ,
    PRIMARY KEY (company_id, group_id, capability),
    FOREIGN KEY (company_id, group_id)
        REFERENCES groups(company_id, id)
);
```

Separate user and group tables provide ordinary composite foreign keys and avoid weakly validated polymorphic subjects. If a unified capability table is preferred, it needs database triggers equivalent to Wikore's current same-company validation.

Capability changes should bump a per-user or per-company authorization epoch so cached decisions cannot survive revocation. Expiry must clamp cache TTL in the same way as other time-limited authorization records.

### Request-time enforcement

The server derives capabilities after authentication. Clients must not supply trusted flags such as:

```json
{
  "relationship_mode_enabled": true,
  "is_director": true,
  "diagnostics": true
}
```

An ordinary caller requesting `retrieval_intent=bridge` without `relationship_search` should receive `403`, or the public chat API may omit that option entirely for unentitled users. Silent fallback to fact retrieval can hide authorization/configuration mistakes and should be used only if explicitly chosen as product behavior.

The UI may hide unavailable controls, but the API must independently enforce every capability.

### Entitlement does not imply evidence access

A user granted `relationship_search` can search relationships only among sources already visible through normal EvidenceGate policy.

Examples:

- A Legal reviewer allowed to read Legal and selected HR sources may retrieve edges connecting those sources.
- A director with relationship-analysis capability but no Finance access must not see Finance relationships or even hidden endpoint metadata.
- A consultant receives capability plus narrowly scoped, expiring resource access for the engagement.
- An administrator can manage feature assignments without automatically receiving restricted-document clearance.

This separation is central to the design:

```text
feature entitlement answers: “may the user perform this type of analysis?”
EvidenceGate answers: “may the user see the evidence involved?”
```

### Default behavior

For users without BaryGraph Lite capabilities:

- wiki and chat continue using ordinary fact retrieval;
- edge Qdrant collections are not searched;
- no graph traversal is performed;
- relationship diagnostics are unavailable;
- relationship metadata is absent from responses;
- no additional relationship-derived information is disclosed.

For entitled users, `automatic` retrieval may search both chunks and relationships. The product may instead require an explicit “relationship analysis” workspace or query mode to control cost and user expectations.

### Diagnostics entitlement

Diagnostics is more sensitive than ordinary relationship search because it observes rejected candidates and policy decisions. Grant it separately to selected operators, security personnel, and developers.

Production diagnostics must remain metadata-only and tenant-bound. Access to diagnostics does not authorize rejected source text. Diagnostic invocation and inspected candidate identifiers must be audited.

During development and CI, fixture-only oracle evaluation may expose complete expected graphs because the data is synthetic and isolated. This does not justify an equivalent production mode.

### Break-glass interaction

Break-glass is an additional temporary scope expansion, not a substitute for feature entitlement.

A production relationship query under break-glass requires:

```text
relationship capability
  AND active break-glass session
  AND approved temporary scope
  AND live all-endpoint EvidenceGate validation
  AND mandatory audit
```

Break-glass should default to analysis without LLM generation. If LLM use is approved, every endpoint must be revalidated before prompt construction and the exact allowed source/edge IDs must be audited.

## Non-negotiable security invariants

### Tenant boundary is never configurable

Every edge, endpoint, query, Qdrant filter, PostgreSQL join, audit record, and cache key must carry `company_id`. No mode, administrator role, diagnostic option, or break-glass session may cross a tenant boundary.

Database-level composite keys and validation triggers should enforce same-company endpoints. Application checks alone are insufficient.

### Every endpoint must be authorized

For an edge with required endpoints `E`:

```text
edge_allowed = edge_policy_allows(edge)
               AND for_all(endpoint in E, evidence_gate_allows(endpoint))
```

For a path with edges `P`:

```text
path_allowed = for_all(edge in P, edge_allowed(edge))
               AND for_all(distinct_endpoint in P, endpoint_allowed(endpoint))
```

If one endpoint is HR confidential and another is Finance internal, a caller must be authorized for both. Showing the edge type or the inaccessible endpoint identifier can itself disclose restricted knowledge.

### PostgreSQL remains authoritative

Qdrant is a recall and ranking index. Edge payloads may be stale. Qdrant may reject candidates early but must never admit a relationship to answer generation.

PostgreSQL must revalidate:

- tenant;
- edge lifecycle;
- endpoint existence;
- endpoint company;
- document/version lifecycle;
- sensitivity clearance;
- resource grants and ownership;
- tombstone/deletion state;
- relationship review status where applicable;
- expiration or supersession.

### No partial relationship hydration

The relationship gate must establish that the whole relationship is allowed before returning any endpoint text. The implementation should avoid constructing a partially hydrated object and subsequently attempting to redact it.

### ContextBuilder accepts only allowed evidence

The `ContextBuilder` API enforces a convention boundary, not a compile-time
guarantee, in the initial implementation:

```cpp
PromptContext build_context(const RequestContext&,
                            std::span<const AllowedEvidence>);
```

It must not overload this function for `ChunkCandidate`, `EdgeCandidate`,
raw Qdrant payloads, arbitrary chunk IDs, or diagnostic records. This makes
accidental mis-use visibly wrong at the call site, though a caller can still
synthesize an `AllowedChunk` or `AllowedRelationship` directly because the
structs have public fields.

For stronger enforcement, make `AllowedChunk`, `AllowedRelationship`, and
`AllowedPath` have private constructors and befriend only `EvidenceGate` /
`RelationshipEvidenceGate` as factory methods. Use
`std::array<AllowedRelationshipEndpoint, 2>` for v1 to make the two-endpoint
cardinality part of the type. This refactor can follow the initial
`ContextBuilder` implementation once the gate interfaces are stable.

### Diagnostics cannot become prompt input

`RetrievalDiagnostics` must be stored separately from allowed evidence. Rejected summaries must not include text, embeddings, full payloads, secret identifiers, or relationship descriptions that reveal inaccessible endpoints.

### Fail closed on authorization and audit failures

Database timeouts, missing scope, malformed edge state, incomplete endpoint sets, unsupported endpoint kinds, and required audit failures must not return partially authorized evidence.

## Proposed C++ domain model

Names are illustrative and should be refined during implementation.

```cpp
namespace wikore::rag {

enum class KnowledgeEdgeType {
    // V1: chunk-to-chunk only. Enforced by knowledge_edges_edge_type_v1_chk.
    cites,
    derived_from,         // admin-authorable in V1 (any two known chunks);
                          // automated creation requires source_chunk_id FK first
    implements,
    depends_on,
    exception_to,
    contradicts,
    same_requirement_as,
    affects,
    requires_approval_from,

    // V2 only: require non-chunk endpoint types.
    // Repository must reject these until typed endpoints exist.
    supersedes,           // version → version
    responsible_team,     // chunk → org_unit
    section_parent_of,    // section → section
    section_contains,     // section → chunk
};

enum class EdgeDirection {
    directed,
    symmetric,
};

enum class EdgeOrigin {
    parser,
    deterministic_rule,
    administrator,
    llm_proposal,
};

enum class EdgeReviewState {
    accepted,
    proposed,
    rejected,
    superseded,
};

struct EdgeCandidate {
    Uuid edge_id;
    float score = 0.0f;
    KnowledgeEdgeType edge_type;
    std::vector<Uuid> endpoint_chunk_ids;
    std::int64_t edge_version = 0;
};

struct AllowedChunk {
    Uuid chunk_id;
    Uuid document_version_id;
    float score = 0.0f;
    std::string text;
    std::optional<std::string> section_heading;
};

struct AllowedRelationshipEndpoint {
    std::string role;
    AllowedChunk evidence;
};

struct AllowedRelationship {
    Uuid edge_id;
    KnowledgeEdgeType edge_type;
    EdgeDirection direction;
    float retrieval_score = 0.0f;
    float confidence = 0.0f;
    EdgeOrigin origin;
    std::vector<AllowedRelationshipEndpoint> endpoints;
};

struct AllowedPath {
    std::vector<AllowedRelationship> edges;
    float path_score = 0.0f;
};

using AllowedEvidence =
    std::variant<AllowedChunk, AllowedRelationship, AllowedPath>;

enum class RetrievalIntent {
    fact,
    bridge,
    automatic,
};

struct RetrievalRequest {
    std::string query;
    Uuid scope_org_unit_id;
    RetrievalIntent intent = RetrievalIntent::automatic;
    int fact_limit = 20;
    int edge_limit = 20;
    int max_path_hops = 2;
};

struct RetrievalResult {
    std::vector<AllowedEvidence> evidence;
    std::optional<RetrievalDiagnostics> diagnostics;
};

} // namespace wikore::rag
```

The current `AllowedCandidate` can either be retained as the concrete chunk type or renamed to `AllowedChunk`. Renaming creates churn; retaining it and introducing `AllowedRelationship` plus `AllowedEvidence` is likely the pragmatic first step.

## Port boundaries

Suggested ports:

```cpp
class RelationshipVectorStorePort {
public:
    virtual ~RelationshipVectorStorePort() = default;

    virtual drogon::Task<Result<std::vector<EdgeCandidate>>>
    search_edges(const Embedding& query,
                 const EdgePrefilter& filter,
                 int limit,
                 double timeout_s) const = 0;
};

class RelationshipRepositoryPort {
public:
    virtual ~RelationshipRepositoryPort() = default;

    virtual drogon::Task<Result<std::vector<KnowledgeEdgeRecord>>>
    load_for_indexing(std::span<const Uuid> edge_ids,
                      postgres::Deadline deadline) const = 0;
};

class RelationshipEvidenceGate {
public:
    virtual ~RelationshipEvidenceGate() = default;

    virtual drogon::Task<Result<RelationshipGateResult>>
    evaluate(const RequestContext& ctx,
             const AccessScope& scope,
             std::span<const std::string> allowed_sensitivity_labels,
             std::span<const EdgeCandidate> candidates,
             postgres::Deadline deadline) const = 0;
};
```

The first implementation may use concrete classes rather than interfaces everywhere, but the logical boundaries should remain.

## PostgreSQL schema proposal

### Why not duplicate left/right document versions

A table containing both `left_chunk_id` and `left_document_version_id` creates two representations of the same fact. They can drift during imports, repair, or faulty writes. The authoritative document version should be resolved through the chunk relationship.

Separate endpoint rows also make endpoint roles explicit and allow later extension beyond exactly two endpoints.

### `knowledge_edges`

```sql
CREATE TABLE knowledge_edges (
    id                  UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    company_id          UUID NOT NULL REFERENCES companies(id) ON DELETE CASCADE,
    edge_type           TEXT NOT NULL,
    direction           TEXT NOT NULL
                            CHECK (direction IN ('directed', 'symmetric')),
    confidence          REAL NOT NULL
                            CHECK (confidence >= 0 AND confidence <= 1),
    origin              TEXT NOT NULL
                            CHECK (origin IN (
                                'parser',
                                'deterministic_rule',
                                'administrator',
                                'llm_proposal'
                            )),
    review_state        TEXT NOT NULL DEFAULT 'accepted'
                            CHECK (review_state IN (
                                'accepted', 'proposed', 'rejected', 'superseded'
                            )),
    provenance          JSONB NOT NULL DEFAULT '{}',
    formula_version     INT NOT NULL DEFAULT 1,
    edge_version        BIGINT NOT NULL DEFAULT 1,
    -- created_by/reviewed_by validated same-company by trigger below.
    created_by          UUID REFERENCES users(id) ON DELETE SET NULL,
    reviewed_by         UUID REFERENCES users(id) ON DELETE SET NULL,
    created_at          TIMESTAMPTZ NOT NULL DEFAULT now(),
    reviewed_at         TIMESTAMPTZ,
    expires_at          TIMESTAMPTZ,
    superseded_at       TIMESTAMPTZ,
    CHECK (expires_at IS NULL OR expires_at > created_at),
    -- Required for composite FK references from child tables.
    UNIQUE (company_id, id)
);

-- Enforce same-company created_by / reviewed_by.
CREATE OR REPLACE FUNCTION knowledge_edges_actors_same_company()
RETURNS TRIGGER LANGUAGE plpgsql AS $$
BEGIN
    IF NEW.created_by IS NOT NULL AND NOT EXISTS (
        SELECT 1 FROM users WHERE id = NEW.created_by AND company_id = NEW.company_id
    ) THEN
        RAISE EXCEPTION 'knowledge_edges.created_by must belong to company %', NEW.company_id
            USING ERRCODE = 'foreign_key_violation',
                  CONSTRAINT = 'knowledge_edges_created_by_same_company_fk';
    END IF;
    IF NEW.reviewed_by IS NOT NULL AND NOT EXISTS (
        SELECT 1 FROM users WHERE id = NEW.reviewed_by AND company_id = NEW.company_id
    ) THEN
        RAISE EXCEPTION 'knowledge_edges.reviewed_by must belong to company %', NEW.company_id
            USING ERRCODE = 'foreign_key_violation',
                  CONSTRAINT = 'knowledge_edges_reviewed_by_same_company_fk';
    END IF;
    RETURN NEW;
END;
$$;

CREATE TRIGGER knowledge_edges_actors_same_company_trg
    BEFORE INSERT OR UPDATE ON knowledge_edges
    FOR EACH ROW EXECUTE FUNCTION knowledge_edges_actors_same_company();
```

The edge-type vocabulary must be enforced by the database for v1. A CHECK
constraint covers the v1 chunk-to-chunk types; types requiring non-chunk
endpoints are explicitly excluded until typed endpoints land in v2:

```sql
-- V1 chunk-to-chunk edge types only.
-- supersedes, section_parent_of, section_contains, responsible_team
-- require non-chunk endpoints and must not be insertable until v2.
ALTER TABLE knowledge_edges
    ADD CONSTRAINT knowledge_edges_edge_type_v1_chk
    CHECK (edge_type IN (
        'implements',
        'depends_on',
        'exception_to',
        'contradicts',
        'same_requirement_as',
        'derived_from',
        'cites',
        'affects',
        'requires_approval_from'
    ));
```

When typed endpoints are introduced in v2, this constraint is dropped and
replaced with a `knowledge_edge_types` reference table. A reference table
is more extensible, but a CHECK constraint is simpler and matches current
Wikore migrations.

### `knowledge_edge_endpoints`

The first release should allow only chunk endpoints:

```sql
CREATE TABLE knowledge_edge_endpoints (
    company_id  UUID     NOT NULL,
    edge_id     UUID     NOT NULL,
    ordinal     SMALLINT NOT NULL CHECK (ordinal IN (0, 1)),
    chunk_id    UUID     NOT NULL,
    role        TEXT     NOT NULL
                    CHECK (role IN ('source','target','subject','object','a','b')),
    PRIMARY KEY (edge_id, ordinal),
    UNIQUE (edge_id, chunk_id),
    -- Composite FK enforces company_id matches the parent edge.
    FOREIGN KEY (company_id, edge_id)
        REFERENCES knowledge_edges(company_id, id) ON DELETE CASCADE,
    FOREIGN KEY (company_id, chunk_id)
        REFERENCES document_chunks(company_id, id) ON DELETE RESTRICT
);

-- Required for "find all edges referencing this chunk" queries:
-- deletion workflow, resync worker, impact analysis. Without this
-- index, WHERE chunk_id = $1 is a full table scan on every chunk delete.
CREATE INDEX knowledge_edge_endpoints_chunk_idx
    ON knowledge_edge_endpoints (company_id, chunk_id);
```

The composite `FOREIGN KEY (company_id, edge_id) REFERENCES knowledge_edges(company_id, id)`
enforces tenant integrity at the database level.

**Two-endpoint CONSTRAINT TRIGGER** — a regular `CREATE TRIGGER` cannot be
deferred; only `CREATE CONSTRAINT TRIGGER ... DEFERRABLE INITIALLY DEFERRED`
can. This is the correct DDL:

```sql
CREATE OR REPLACE FUNCTION knowledge_edges_check_endpoint_count_fn()
RETURNS TRIGGER LANGUAGE plpgsql AS $$
DECLARE
    v_edge_id UUID;
BEGIN
    -- NEW is unassigned in a DELETE trigger; dispatch on TG_OP.
    v_edge_id := CASE TG_OP WHEN 'DELETE' THEN OLD.edge_id ELSE NEW.edge_id END;

    -- Skip edges deleted in this same transaction.
    IF NOT EXISTS (SELECT 1 FROM knowledge_edges WHERE id = v_edge_id) THEN
        RETURN COALESCE(NEW, OLD);
    END IF;
    IF (SELECT count(*) FROM knowledge_edge_endpoints WHERE edge_id = v_edge_id) <> 2 THEN
        RAISE EXCEPTION 'edge % must have exactly two endpoints', v_edge_id
            USING ERRCODE = 'check_violation',
                  CONSTRAINT = 'knowledge_edges_exactly_two_endpoints_chk';
    END IF;
    RETURN COALESCE(NEW, OLD);
END;
$$;

CREATE CONSTRAINT TRIGGER knowledge_edges_exactly_two_endpoints_trg
    AFTER INSERT OR DELETE ON knowledge_edge_endpoints
    DEFERRABLE INITIALLY DEFERRED
    FOR EACH ROW EXECUTE FUNCTION knowledge_edges_check_endpoint_count_fn();
```

The trigger fires at COMMIT (not per-INSERT), so both endpoint inserts
complete before the count is checked. The trigger skips edges whose parent
row was deleted in the same transaction (cascade delete).

**Deletion ordering — BEFORE DELETE trigger captures both history and outbox:**
Steps 1–4 in the recommended deletion flow (SELECT point IDs → INSERT outbox →
DELETE edge → COMMIT) rely on application discipline. A safer complement is
a `BEFORE DELETE` trigger on `knowledge_edges` that captures `qdrant_point_id`
values from `knowledge_edge_embeddings` AND inserts the outbox event atomically,
so DBA direct-delete and repair jobs are also covered (see Edge history section).

### `knowledge_edge_embeddings`

```sql
CREATE TABLE knowledge_edge_embeddings (
    company_id           UUID        NOT NULL,
    edge_id              UUID        NOT NULL,
    embedding_model_id   UUID        NOT NULL
                             REFERENCES embedding_models(id) ON DELETE RESTRICT,
    qdrant_point_id      UUID        NOT NULL,
    formula_version      INT         NOT NULL,
    indexed_edge_version BIGINT      NOT NULL,
    indexed_at           TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (edge_id, embedding_model_id),
    -- Composite FK enforces company_id matches the parent edge.
    FOREIGN KEY (company_id, edge_id)
        REFERENCES knowledge_edges(company_id, id) ON DELETE CASCADE,
    UNIQUE (embedding_model_id, qdrant_point_id)
);
```

`embedding_model_id` references the existing `embedding_models` table
introduced in `V003__documents.sql` and used by `document_chunk_vectors`.
`indexed_edge_version` lets the scheduler identify stale vectors after
confidence, type, endpoint, or review-state changes.

### Edge history (mandatory when hard-delete is used)

If edges can be hard-deleted (the recommended v1 deletion strategy),
governance provenance disappears with the deleted row. Any audit query
for "what relationships existed at this point in time" becomes
unanswerable once the rows are gone — including the endpoint chunk IDs,
which CASCADE removes along with the edge.

Edge history is therefore **mandatory before implementing hard-delete**.
Add `knowledge_edges_history` using the same pattern as
`memberships_history` (V024): `live_row_id` is a plain `UUID NOT NULL`
with no FK, matching the live-row-id-goes-stale-on-delete design:

```sql
CREATE TABLE knowledge_edges_history (
    -- Plain UUID, no FK — live_row_id becomes a stale identifier after
    -- the knowledge_edges row is deleted, matching memberships_history.
    live_row_id     UUID        NOT NULL,
    edge_id         UUID        NOT NULL,   -- stable even after deletion
    company_id      UUID        NOT NULL,
    edge_type       TEXT        NOT NULL,
    direction       TEXT        NOT NULL,
    confidence      REAL        NOT NULL,
    origin          TEXT        NOT NULL,
    review_state    TEXT        NOT NULL,
    provenance      JSONB       NOT NULL,
    formula_version INT         NOT NULL,
    edge_version    BIGINT      NOT NULL,
    created_by      UUID,
    reviewed_by     UUID,
    -- Endpoint snapshot: captured from knowledge_edge_endpoints BEFORE
    -- CASCADE deletes them. Both chunk UUIDs are required; without them
    -- the history record cannot answer "which chunks were connected."
    endpoint_0_chunk_id  UUID  NOT NULL,
    endpoint_1_chunk_id  UUID  NOT NULL,
    endpoint_0_role      TEXT  NOT NULL,
    endpoint_1_role      TEXT  NOT NULL,
    change_kind     TEXT        NOT NULL CHECK (change_kind IN ('insert','update','delete')),
    valid_from      TIMESTAMPTZ NOT NULL,
    valid_until     TIMESTAMPTZ,
    UNIQUE (live_row_id, valid_from)
);

-- One open interval per edge at most — matches memberships_history pattern.
-- Prevents concurrent endpoint updates from leaving multiple open rows.
CREATE UNIQUE INDEX knowledge_edges_history_open_uidx
    ON knowledge_edges_history (live_row_id)
    WHERE valid_until IS NULL;
```

The history trigger is a `BEFORE DELETE` trigger on `knowledge_edges`.
It must execute **before** CASCADE fires. Three precise requirements:

**1. BEFORE DELETE: close open interval, insert delete marker, AND capture Qdrant point IDs into outbox.**
The trigger handles both history and Qdrant cleanup atomically — this makes
direct-DELETE from psql (incident response, repair jobs) safe without
relying on application-layer discipline.
Use `clock_timestamp()` to avoid `now()` transaction-time collisions.

```sql
CREATE OR REPLACE FUNCTION knowledge_edges_history_delete_fn()
RETURNS TRIGGER LANGUAGE plpgsql AS $$
DECLARE
    ep0        knowledge_edge_endpoints%ROWTYPE;
    ep1        knowledge_edge_endpoints%ROWTYPE;
    point_ids  UUID[];
    ts         TIMESTAMPTZ := clock_timestamp();
BEGIN
    -- Capture endpoints BEFORE CASCADE removes them.
    SELECT * INTO ep0 FROM knowledge_edge_endpoints
        WHERE edge_id = OLD.id AND ordinal = 0;
    SELECT * INTO ep1 FROM knowledge_edge_endpoints
        WHERE edge_id = OLD.id AND ordinal = 1;

    -- Capture Qdrant point IDs BEFORE cascade removes embedding rows.
    SELECT array_agg(qdrant_point_id) INTO point_ids
        FROM knowledge_edge_embeddings WHERE edge_id = OLD.id;

    -- Emit outbox event for Qdrant cleanup so deletion from any caller
    -- (application, psql, repair job) is safe.
    -- Column names from V015: job_type (not event_type), idempotency_key NOT NULL.
    IF point_ids IS NOT NULL AND array_length(point_ids, 1) > 0 THEN
        INSERT INTO outbox_events (company_id, aggregate_id, job_type,
                                   job_schema_version, payload, idempotency_key)
        VALUES (OLD.company_id, OLD.id, 'qdrant_delete_edge_points', 1,
                jsonb_build_object('edge_id', OLD.id,
                                   'qdrant_point_ids', to_jsonb(point_ids)),
                'edge-del:' || OLD.id::text || ':' || OLD.edge_version::text);
    END IF;

    -- Close the currently-open interval.
    UPDATE knowledge_edges_history
        SET valid_until = ts
        WHERE live_row_id = OLD.id AND valid_until IS NULL;

    -- Insert the delete marker (valid_until = valid_from = zero-width interval).
    INSERT INTO knowledge_edges_history (
        live_row_id, edge_id, company_id, edge_type, direction,
        confidence, origin, review_state, provenance,
        formula_version, edge_version, created_by, reviewed_by,
        endpoint_0_chunk_id, endpoint_1_chunk_id,
        endpoint_0_role, endpoint_1_role,
        change_kind, valid_from, valid_until
    ) VALUES (
        OLD.id, OLD.id, OLD.company_id, OLD.edge_type, OLD.direction,
        OLD.confidence, OLD.origin, OLD.review_state, OLD.provenance,
        OLD.formula_version, OLD.edge_version, OLD.created_by, OLD.reviewed_by,
        ep0.chunk_id, ep1.chunk_id,
        ep0.role, ep1.role,
        'delete', ts, ts
    );
    RETURN OLD;
END;
$$;

CREATE TRIGGER knowledge_edges_history_before_delete
    BEFORE DELETE ON knowledge_edges
    FOR EACH ROW EXECUTE FUNCTION knowledge_edges_history_delete_fn();
```

Note: "current live edge" queries must use `valid_until IS NULL`, not
`valid_until > now()`, because delete markers have `valid_until = valid_from`
(a zero-width past interval).

**2. INSERT: endpoints don't exist yet when the edge AFTER INSERT fires.**
The repository inserts the edge row, then both endpoint rows. An
`AFTER INSERT ON knowledge_edges` trigger fires immediately — before the
endpoints are inserted — so it cannot snapshot the chunk IDs.

Use an `AFTER INSERT ON knowledge_edge_endpoints WHERE ordinal = 1`
trigger instead. By the repository invariant (both endpoints inserted in
one transaction), ordinal=1 being inserted signals that ordinal=0 is
already present and the history snapshot can be taken:

```sql
CREATE OR REPLACE FUNCTION knowledge_edge_endpoints_history_insert_fn()
RETURNS TRIGGER LANGUAGE plpgsql AS $$
DECLARE
    edge  knowledge_edges%ROWTYPE;
    ep0   knowledge_edge_endpoints%ROWTYPE;
    ts    TIMESTAMPTZ := clock_timestamp();
BEGIN
    -- Only fire on the second endpoint (ordinal=1); ordinal=0 is already present
    -- by the repository insertion-order invariant.
    IF NEW.ordinal <> 1 THEN RETURN NEW; END IF;

    -- Use STRICT: silently-NULL ep0 would violate endpoint_0_chunk_id NOT NULL
    -- and produce a cryptic 23502 error. STRICT raises NO_DATA_FOUND instead,
    -- surfacing the real problem (ordinal=0 not yet inserted) with a clear message.
    SELECT * INTO STRICT edge FROM knowledge_edges WHERE id = NEW.edge_id;
    SELECT * INTO STRICT ep0  FROM knowledge_edge_endpoints
        WHERE edge_id = NEW.edge_id AND ordinal = 0;

    INSERT INTO knowledge_edges_history (
        live_row_id, edge_id, company_id, edge_type, direction,
        confidence, origin, review_state, provenance,
        formula_version, edge_version, created_by, reviewed_by,
        endpoint_0_chunk_id, endpoint_1_chunk_id,
        endpoint_0_role, endpoint_1_role,
        change_kind, valid_from, valid_until
    ) VALUES (
        edge.id, edge.id, edge.company_id, edge.edge_type, edge.direction,
        edge.confidence, edge.origin, edge.review_state, edge.provenance,
        edge.formula_version, edge.edge_version, edge.created_by, edge.reviewed_by,
        ep0.chunk_id, NEW.chunk_id,
        ep0.role, NEW.role,
        'insert', ts, NULL
    );
    RETURN NEW;
END;
$$;

CREATE TRIGGER knowledge_edge_endpoints_history_after_insert
    AFTER INSERT ON knowledge_edge_endpoints
    FOR EACH ROW EXECUTE FUNCTION knowledge_edge_endpoints_history_insert_fn();
```

**3. AFTER UPDATE on edge or endpoint: shared helper, two triggers.**
Refactor the snapshot logic into a plain `PROCEDURE` (or `FUNCTION` with
no trigger signature) that accepts `edge_id` and `change_kind`. Both the
edge-update trigger and the endpoint-update trigger call it directly:

```sql
-- Shared snapshot helper — not a trigger function.
CREATE OR REPLACE PROCEDURE knowledge_edges_snapshot(
    p_edge_id   UUID,
    p_kind      TEXT,
    p_ts        TIMESTAMPTZ
) LANGUAGE plpgsql AS $$
DECLARE
    edge knowledge_edges%ROWTYPE;
    ep0  knowledge_edge_endpoints%ROWTYPE;
    ep1  knowledge_edge_endpoints%ROWTYPE;
BEGIN
    SELECT * INTO edge FROM knowledge_edges WHERE id = p_edge_id;
    SELECT * INTO ep0  FROM knowledge_edge_endpoints
        WHERE edge_id = p_edge_id AND ordinal = 0;
    SELECT * INTO ep1  FROM knowledge_edge_endpoints
        WHERE edge_id = p_edge_id AND ordinal = 1;

    UPDATE knowledge_edges_history
        SET valid_until = p_ts
        WHERE live_row_id = p_edge_id AND valid_until IS NULL;

    INSERT INTO knowledge_edges_history (
        live_row_id, edge_id, company_id, edge_type, direction,
        confidence, origin, review_state, provenance,
        formula_version, edge_version, created_by, reviewed_by,
        endpoint_0_chunk_id, endpoint_1_chunk_id,
        endpoint_0_role, endpoint_1_role,
        change_kind, valid_from, valid_until
    ) VALUES (
        edge.id, edge.id, edge.company_id, edge.edge_type, edge.direction,
        edge.confidence, edge.origin, edge.review_state, edge.provenance,
        edge.formula_version, edge.edge_version, edge.created_by, edge.reviewed_by,
        ep0.chunk_id, ep1.chunk_id,
        ep0.role, ep1.role,
        p_kind, p_ts, NULL
    );
END;
$$;

-- Edge metadata updated.
CREATE OR REPLACE FUNCTION knowledge_edges_history_update_fn()
RETURNS TRIGGER LANGUAGE plpgsql AS $$
BEGIN
    CALL knowledge_edges_snapshot(NEW.id, 'update', clock_timestamp());
    RETURN NEW;
END;
$$;

CREATE TRIGGER knowledge_edges_history_after_update
    AFTER UPDATE ON knowledge_edges
    FOR EACH ROW EXECUTE FUNCTION knowledge_edges_history_update_fn();

-- Endpoint role changed (chunk_id is immutable; role is the primary update path).
CREATE OR REPLACE FUNCTION knowledge_edge_endpoints_history_update_fn()
RETURNS TRIGGER LANGUAGE plpgsql AS $$
BEGIN
    CALL knowledge_edges_snapshot(NEW.edge_id, 'update', clock_timestamp());
    RETURN NEW;
END;
$$;

CREATE TRIGGER knowledge_edge_endpoints_history_after_update
    AFTER UPDATE ON knowledge_edge_endpoints
    FOR EACH ROW EXECUTE FUNCTION knowledge_edge_endpoints_history_update_fn();
```

`knowledge_edges_snapshot` is also callable from application code and
migration scripts for explicit snapshots outside a trigger context.

**History snapshot approach: explicit repository control, not trigger composition.**

Two separate `UPDATE` statements in one transaction — one on `knowledge_edges`,
one on `knowledge_edge_endpoints` — each fire their trigger at depth 1
independently. The second `knowledge_edges_snapshot` call correctly runs
`UPDATE ... SET valid_until = ts WHERE valid_until IS NULL` first (closing
the interval the first call opened), then inserts a new open row. No unique-index
violation. The triggers compose correctly.

The `pg_trigger_depth()` guard in the previous version of this document was
based on incorrect analysis. Two separate statements both at depth 1 correctly
transition history state — the guard would silently suppress legitimate
endpoint-only snapshots.

However, trigger-based automatic snapshotting has a deeper problem: it is
difficult to reason about under composition as the schema grows (nested triggers,
bulk operations, migration scripts). The preferred approach is **explicit
repository-controlled snapshotting**:

- Remove the `AFTER UPDATE ON knowledge_edges` and `AFTER UPDATE ON knowledge_edge_endpoints` triggers entirely.
- The repository method (or stored procedure) that performs a review workflow
  explicitly calls `knowledge_edges_snapshot(id, 'update', clock_timestamp())`
  after all mutations are complete, once, predictably:

```sql
-- Repository-level review workflow (example).
BEGIN;
    UPDATE knowledge_edges
        SET review_state = 'accepted', reviewed_by = $reviewer, reviewed_at = now(),
            edge_version = edge_version + 1
        WHERE id = $edge_id AND company_id = $company_id;
    UPDATE knowledge_edge_endpoints
        SET role = $new_role
        WHERE edge_id = $edge_id AND ordinal = $ordinal;
    CALL knowledge_edges_snapshot($edge_id, 'update', clock_timestamp());
COMMIT;
```

Only the AFTER INSERT (ordinal=1) trigger and the BEFORE DELETE trigger remain
as automatic triggers, because those have precisely-scoped, single-caller
semantics. Update history is always explicit.

The impact query path ("which edges referenced this now-deleted chunk?")
queries `knowledge_edges_history` on `endpoint_0_chunk_id` or
`endpoint_1_chunk_id` when the live rows are gone.

### Privileged and high-sensitivity access schema

Wikore's existing append-only `audit_log` is the foundation, but it does not by itself create a privileged-access workflow or guarantee that every sensitive source view is recorded. Database work for BaryGraph Lite should include explicit privileged-access sessions, approvals, scopes, and an audit-event contract.

This facility covers:

- temporary consultant and legal-review engagements;
- high-sensitivity relationship or impact analysis;
- approved restricted-source access if Wikore later supports it;
- emergency break-glass access;
- production retrieval diagnostics involving sensitive metadata.

Ordinary long-lived relationship-analysis entitlement does not require a privileged session when all endpoints are already within the user's normal evidence scope. A privileged session is required when access is temporary, purpose-bound, unusually sensitive, or expands the user's normal evidence scope.

#### `privileged_access_sessions`

```sql
CREATE TABLE privileged_access_sessions (
    id                  UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    company_id          UUID NOT NULL REFERENCES companies(id) ON DELETE CASCADE,
    subject_user_id     UUID NOT NULL,
    requested_by        UUID NOT NULL,
    purpose             TEXT NOT NULL CHECK (length(btrim(purpose)) >= 10),
    access_kind         TEXT NOT NULL CHECK (access_kind IN (
                            'temporary_engagement',
                            'sensitive_analysis',
                            'retrieval_diagnostics',
                            'break_glass'
                        )),
    status              TEXT NOT NULL DEFAULT 'pending' CHECK (status IN (
                            'pending', 'approved', 'active', 'expired',
                            'revoked', 'rejected'
                        )),
    starts_at           TIMESTAMPTZ NOT NULL,
    expires_at          TIMESTAMPTZ NOT NULL,
    allow_llm           BOOLEAN NOT NULL DEFAULT false,
    require_dual_approval BOOLEAN NOT NULL DEFAULT false,
    activated_at        TIMESTAMPTZ,
    revoked_at          TIMESTAMPTZ,
    revoked_by          UUID,
    revocation_reason   TEXT,
    created_at          TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at          TIMESTAMPTZ NOT NULL DEFAULT now(),
    CHECK (expires_at > starts_at),
    CHECK (expires_at <= starts_at + INTERVAL '24 hours'),
    CHECK ((revoked_at IS NULL) = (revoked_by IS NULL)),
    FOREIGN KEY (company_id, subject_user_id)
        REFERENCES users(company_id, id),
    FOREIGN KEY (company_id, requested_by)
        REFERENCES users(company_id, id),
    FOREIGN KEY (company_id, revoked_by)
        REFERENCES users(company_id, id)
);
```

The 24-hour database maximum is illustrative. Tenant policy may impose a shorter maximum, such as 30 minutes for break-glass and one working day for an approved legal review. Extensions should create a new approval decision rather than silently moving `expires_at` indefinitely.

The table needs a trigger enforcing legal state transitions. For example, `revoked` cannot return to `active`, `rejected` cannot activate, and an expired session cannot be extended in place.

#### `privileged_access_approvals`

```sql
CREATE TABLE privileged_access_approvals (
    session_id          UUID NOT NULL,
    company_id          UUID NOT NULL,
    approver_user_id    UUID NOT NULL,
    decision            TEXT NOT NULL CHECK (decision IN ('approved', 'rejected')),
    reason              TEXT NOT NULL CHECK (length(btrim(reason)) >= 5),
    decided_at          TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (session_id, approver_user_id),
    FOREIGN KEY (session_id)
        REFERENCES privileged_access_sessions(id) ON DELETE RESTRICT,
    FOREIGN KEY (company_id, approver_user_id)
        REFERENCES users(company_id, id)
);
```

A validation trigger must ensure approval company matches session company. It must also enforce separation of duties according to tenant policy:

- the subject cannot approve their own access;
- the requester cannot be the sole approver for dual-approval sessions;
- the same person cannot satisfy two approval slots;
- approvers must hold the required approval capability at decision time.

Approval rows should be immutable. A changed decision is a new workflow/session, not an UPDATE that rewrites history.

#### `privileged_access_scopes`

```sql
CREATE TABLE privileged_access_scopes (
    session_id          UUID NOT NULL,
    company_id          UUID NOT NULL,
    org_unit_id         UUID NOT NULL,
    applies_to          TEXT NOT NULL CHECK (applies_to IN (
                            'self_only', 'self_and_descendants'
                        )),
    maximum_sensitivity TEXT NOT NULL CHECK (maximum_sensitivity IN (
                            'public', 'internal', 'confidential', 'restricted'
                        )),
    allow_relationships BOOLEAN NOT NULL DEFAULT true,
    allow_impact_analysis BOOLEAN NOT NULL DEFAULT false,
    allow_diagnostics   BOOLEAN NOT NULL DEFAULT false,
    PRIMARY KEY (session_id, org_unit_id),
    FOREIGN KEY (session_id)
        REFERENCES privileged_access_sessions(id) ON DELETE RESTRICT,
    FOREIGN KEY (company_id, org_unit_id)
        REFERENCES org_units(company_id, id)
);
```

The scope table may later require explicit document, matter, region, jurisdiction, or legal-hold scopes. Do not encode these as an unrestricted JSON predicate. Each scope kind should have validated columns and same-company foreign keys or a strongly validated companion table.

For multinational tenants, scope may also need:

- permitted legal entities;
- permitted data regions;
- jurisdiction;
- matter or engagement identifier;
- export restrictions;
- purpose limitation;
- retention deadline.

#### Audit event contract

The existing `audit_log.action` column is unconstrained and can accept the required event namespaces without altering old rows. Add and consistently use actions such as:

```text
privileged_access.requested
privileged_access.approved
privileged_access.rejected
privileged_access.activated
privileged_access.expired
privileged_access.revoked
sensitive_access.requested
sensitive_access.denied
sensitive_access.started
sensitive_access.source_viewed
sensitive_access.relationship_viewed
sensitive_access.path_viewed
sensitive_access.completed
sensitive_access.cancelled
retrieval.shadow_diagnostics.used
```

For high-sensitivity access, `audit_log.detail` should record identifiers and policy decisions, never raw source text or prompts:

```json
{
  "privileged_session_id": "uuid",
  "request_trace_id": "uuid",
  "purpose": "Approved acquisition policy impact review",
  "access_kind": "sensitive_analysis",
  "maximum_sensitivity": "restricted",
  "retrieval_intent": "bridge",
  "scope_org_unit_ids": ["uuid"],
  "source_chunk_ids": ["uuid"],
  "document_version_ids": ["uuid"],
  "knowledge_edge_ids": ["uuid"],
  "path_edge_ids": ["uuid"],
  "outcome": "allowed",
  "llm_used": false
}
```

The exact user email and relevant organization/matter names should be denormalized where audit retention requires attribution after account deletion. Sensitive human-readable names should be minimized according to audit-access policy.

#### Fail-closed database flow

Sensitive retrieval should use a transaction-backed authorization flow:

1. Lock or read the privileged session under a consistent snapshot.
2. Verify tenant, subject, status, start, expiry, approvals, and capability.
3. Resolve approved temporary scopes.
4. Run the authoritative all-endpoint EvidenceGate.
5. Insert `sensitive_access.started` with the exact allowed source and edge IDs.
6. Commit before any source text is released to the caller or sent to an LLM.

If steps 1–5 fail, return no sensitive evidence. Logging failure is therefore an authorization failure, not a best-effort telemetry problem.

After delivery or streaming, append a completion, cancellation, or failure event. A network failure can prevent a perfect “viewed by human” determination, so the pre-release event should mean “evidence released to response/LLM pipeline,” while the terminal event records the observed delivery outcome.

For LLM-enabled privileged sessions, the pre-release audit must be committed before prompt construction. The prompt builder must receive only the source and relationship IDs recorded in that event.

#### Immutability and database roles

The current `audit_log` mutation-prevention triggers should continue to cover all sensitive-access actions. Deployment must also enforce the documented application-role restriction: INSERT permission without UPDATE or DELETE permission on audit partitions.

Additional controls:

- approval rows are append-only;
- privileged-session transitions are written to a history table or transition log;
- scope rows cannot change after activation;
- revocation is an explicit transition, not deletion;
- audit partitions and overflow are monitored;
- an external tamper-evident audit sink may consume outbox events for regulated tenants;
- audit-reader permissions are separate from relationship-search and break-glass permissions;
- audit access is itself audited.

#### Cache and revocation behavior

Activation, scope change, expiry, and revocation must invalidate cached authorization decisions. Add a privileged-access epoch or include the session ID/version and expiry in every relevant cache key.

Cache TTL must be clamped to the earliest of:

- normal access-scope expiry;
- capability-grant expiry;
- privileged-session expiry;
- temporary scope expiry;
- request deadline.

Revocation must bump the appropriate epoch and fail closed even if Qdrant relationship payloads or Redis scope entries are stale.

#### Retention and audit volume

Per-source and per-edge audit events can be high volume. Store one event per retrieval/answer with bounded arrays of exact evidence IDs rather than one row for every chunk where that preserves the required audit semantics. If arrays can exceed a safe row size, use an append-only child table keyed by the audit event.

Retention must reflect sensitivity, legal hold, tenant policy, and jurisdiction. Audit deletion, if legally required, must use a controlled retention process rather than granting the application role ordinary DELETE permission.

## Edge vector construction

### Base formula (BaryGraph reference)

For two endpoint vectors `left`, `right`, relation-type vector `type`, and quality `q`:

```text
edge_vector = normalize(q * left
                      + q * right
                      + (1 - q) * type)
```

This is the BaryGraph L14/L15 formula. Both endpoints are weighted equally
by `q`. The formula and all coefficients must be versioned. A vector
written with one formula must not be interpreted as another.

**Critical property:** `v_type` always receives weight `(1 - q)`. Setting
`q = 1` for deterministic edges eliminates the type vector entirely,
making `implements`, `contradicts`, and `depends_on` indistinguishable
when connecting the same endpoints. The BaryGraph PoC avoids this by
assigning per-edge-type `q_seed` values below 1.0 at L14 (e.g.
`contradicts = 0.85`, `synonyms = 0.90`) so the type vector always
contributes. Wikore must preserve this property.

### Wikore-adapted formula (formula_version = 1)

Wikore already stores `documents.authority_level` (0–100) for every
document. For enterprise edges, endpoint authority is often asymmetric:
a policy chunk (authority = 90) implementing a procedure chunk
(authority = 50) should produce a vector weighted toward the source of
truth.

The key correction from the BaryGraph PoC: **`q` must not drive the
type-vector contribution to zero**. Formula version 1 uses fixed
`w_ep = 0.65` and `w_type = 0.35` per formula version, with authority
values normalized so the combined endpoint mass is exactly 0.65:

```text
-- authority_level is NOT NULL DEFAULT 50, CHECK BETWEEN 0 AND 100.
-- 50/50 fallback only when both endpoints are exactly zero.
-- Authority floor of 10 prevents a single authority=0 endpoint from
-- contributing zero mass to the edge vector (which would make all edges
-- involving that chunk semantically identical modulo the type vector).
-- floor=10 means the minimum endpoint share is 10/(10+100) ≈ 9%.
FLOOR = 10
a_source = (max(authority_source, FLOOR)) / (max(authority_source, FLOOR) + max(authority_target, FLOOR))
a_target = (max(authority_target, FLOOR)) / (max(authority_source, FLOOR) + max(authority_target, FLOOR))

edge_vector = normalize(
    0.65 * (a_source * v_source + a_target * v_target)
  + 0.35 * v_type
)
```

The zero-sum fallback (`50/50` when both are exactly zero) remains as a
safety net but is now unreachable given the floor. Both values are snapshotted
in provenance at indexing time. `FLOOR` is a formula constant versioned with
`formula_version`.

`confidence` (edge truthfulness) is stored separately and affects
ranking and admission — it does **not** affect `w_type`. `implements`,
`contradicts`, and `depends_on` always contribute their type semantics
to the vector regardless of how certain we are the edge is correct.

Authority values are snapshotted from `documents.authority_level` at
indexing time and stored in provenance so they can be reproduced if
documents are re-indexed with different authority levels later.

Formula version 2 may introduce separate `w_source` ≠ `w_target` for
explicitly directed types. Both formula versions produce vectors in the
same embedding space, but point IDs incorporate formula version so they
never collide.

### Separate `confidence`, `w_ep/w_type`, `authority`, and `retrieval_score`

These are four distinct quantities that must not be conflated:

| Field | Meaning | Where |
|---|---|---|
| `w_ep`, `w_type` | Formula weight constants for this formula version | Versioned formula constant |
| `authority_source/target` | Snapshotted `documents.authority_level` at index time | `provenance` JSONB |
| `confidence` | Reviewable edge truthfulness (human or model) | `knowledge_edges.confidence` |
| `retrieval_score` | Qdrant cosine similarity at query time | Never stored; computed per-query |

`w_ep` and `w_type` are formula constants, not per-edge values.
`confidence` affects ranking and admission. Neither controls the other.

### `confidence` derivation by origin

| Origin | `confidence` value |
|---|---|
| `deterministic_rule` | `1.0` — structural fact |
| `administrator` | `0.95` — human-confirmed |
| `parser` — citation, cross-reference, heading | extraction confidence from provenance |
| `llm_proposal`, `review_state = accepted` | `model_confidence × acceptance_quality_factor` |
| `llm_proposal`, `review_state = proposed` | **not indexed** — held out of Qdrant |
| `llm_proposal`, `review_state = rejected` | removed from Qdrant |

Store all inputs (`origin`, `model_confidence`, `review_state`,
`acceptance_quality_factor`) in the `provenance` JSONB column.

### Relation-type vectors

Type vectors are generated from stable, product-owned descriptions
embedded **once per formula version during an admin indexing job**, not
at API startup. API startup must not depend on the embedding service or
regenerate semantic configuration. The BaryGraph PoC embeds type vectors
during its construction pipeline stage (`s06_l14_edges.py`), not at
service startup.

The indexing job stores type vectors in a dedicated registry table or
configuration blob keyed by `(formula_version, edge_type)`. The API
reads pre-computed vectors from that store.

```cpp
// Source descriptions — embedded by the admin indexing job.
// Changes to any string require formula_version increment + reindex.
static constexpr std::pair<KnowledgeEdgeType, std::string_view>
kTypeDescriptions[] = {
    // V1 types: chunk-to-chunk, enforced by knowledge_edges_edge_type_v1_chk.
    {KnowledgeEdgeType::implements,
     "one governed procedure operationalises and fulfils the other requirement"},
    {KnowledgeEdgeType::depends_on,
     "this process cannot proceed without the other process or system"},
    {KnowledgeEdgeType::exception_to,
     "this source states a bounded, scoped exception to the target rule"},
    {KnowledgeEdgeType::contradicts,
     "these two governed statements impose incompatible requirements"},
    {KnowledgeEdgeType::same_requirement_as,
     "these two statements express the same regulatory or policy requirement"},
    {KnowledgeEdgeType::derived_from,
     "the source content is derived from or authored based on the target"},
    {KnowledgeEdgeType::cites,
     "the source explicitly references and relies on the target as authority"},
    {KnowledgeEdgeType::affects,
     "a change to the source directly affects the scope or validity of the target"},
    {KnowledgeEdgeType::requires_approval_from,
     "the source action requires explicit authorisation from the target authority"},

    // V2 types: require typed endpoints; not inserted until v2 schema lands.
    {KnowledgeEdgeType::supersedes,
     "the source document replaces and invalidates the target document"},
    {KnowledgeEdgeType::responsible_team,
     "the source process or policy is owned or executed by the target team"},
    {KnowledgeEdgeType::section_parent_of,
     "the source section contains the target as a direct child subsection"},
    {KnowledgeEdgeType::section_contains,
     "the source section directly contains the target chunk as its content"},
};
```

### Deterministic point identifiers

```text
qdrant_point_id = uuid_v5(
    edge_id + ":" + str(embedding_model_id)
    + ":" + str(formula_version)
    + ":" + str(edge_version)
)
```

`embedding_model_id` is the UUID FK from the existing `embedding_models`
table introduced in `V003__documents.sql`. Wikore already has this registry;
`knowledge_edge_embeddings` should reference it with the same FK pattern
used by `document_chunk_vectors`. Outbox event payloads may continue
carrying the model name string for observability.

## Wikore-adapted design rationale

This section documents the design decisions that depart from the BaryGraph
PoC and the corrections made after review of the source and schema.

### V1 edges are strictly chunk-to-chunk

The `knowledge_edge_endpoints` schema supports only chunk endpoints at
v1. Several of the proposed deterministic edge types require non-chunk
endpoints:

- `supersedes`: version → version (document_versions, not chunks)
- `section_parent_of`: section → section (document_sections, not chunks)
- `cites`: wiki page version → source chunk or document version
- `responsible_team`: chunk → org unit

For v1, these relationships should be handled by existing native tables
and foreign keys that already model them correctly:

| Relationship | Existing model |
|---|---|
| Version supersedes version | `document_versions.superseded_at`, existing FK |
| Section hierarchy | `document_sections.parent_section_id`, existing self-FK |
| Wiki page cites document/chunk | `wiki_page_sources` join table (V004) |
| Document owner/org | `documents.owner_org_unit_id` |

These do not need to become `knowledge_edges` rows in v1. They can be
traversed by the impact analysis queries directly from their native tables.

V1 `knowledge_edges` contains only semantic chunk-to-chunk
relationships: `implements`, `depends_on`, `exception_to`, `contradicts`,
`same_requirement_as`, `derived_from`, `cites` (chunk → chunk only),
`affects`, `requires_approval_from`.

`section_contains` (section → chunk) and `section_parent_of`
(section → section) require a section endpoint type and belong in v2
alongside typed endpoints. In v1, section-to-chunk membership is
resolved via `document_chunks.section_id → document_sections`.

### Enterprise knowledge is a directed multigraph, not a DAG

The claim that "enterprise knowledge is a DAG" is too strong. Several
real configurations produce cycles or symmetric structures:

- `contradicts` is a symmetric relationship (A contradicts B = B contradicts A)
- procedure dependency chains can be circular in practice (A depends_on B, B depends_on A for a shared resource)
- the same two chunks can be connected by multiple edges of different types simultaneously

Wikore's edge graph is a **directed multigraph**: directed edges, multiple
edge types permitted between the same object pair, no acyclicity guarantee.

Consequences:
- Bounded traversal (max hops, max nodes, cycle detection by visited edge
  ID set) is mandatory — do not assume acyclicity to justify skipping it.
- Acyclicity can be enforced only for specific structural types like
  `section_parent_of` (a section cannot be its own ancestor) via a
  deferred constraint or an application check at creation time.
- `contradicts` and `same_requirement_as` should be stored in canonical
  endpoint order (lower UUID first) and treated as symmetric at query time.

### Transactional endpoint enforcement with deferred constraint

Application-only enforcement of the exactly-two-endpoints invariant
(via a stored procedure plus periodic reconciliation) is weaker than a
properly scoped database constraint. A network failure or application bug
between endpoint inserts can leave a one-endpoint edge that passes the
reconciliation window before being detected.

The correct approach when the repository owns one transaction boundary:

1. Repository method or stored procedure inserts the edge and both
   endpoints in a single transaction.
2. A deferred `INITIALLY DEFERRED` trigger validates endpoint count at
   COMMIT, not at INSERT. Both endpoints must be inserted before COMMIT.
3. Periodic reconciliation as defense-in-depth.

The deferred trigger fires at COMMIT only — it does not fire after the
first endpoint INSERT in the same transaction. This eliminates the
premature-constraint-fire problem as long as both inserts happen in one
transaction, which the repository must guarantee.

### Hard-delete and tombstone behavior for endpoints

`ON DELETE RESTRICT` on `knowledge_edge_endpoints.chunk_id` does not
block archive operations — archive updates `lifecycle_status`, not the
chunk row. It can block:

- hard deletion of orphaned chunks by the cleanup job (`src/ingest/document_repo.cpp:404`)
- any future compaction that physically deletes old chunk rows

`ON DELETE SET NULL` is incompatible with `chunk_id NOT NULL`.

The team must choose one of three explicit strategies before writing
the migration:

| Strategy | Mechanism | Trade-off |
|---|---|---|
| **Delete edge + cascade (recommended)** | Capture Qdrant point IDs, INSERT outbox event, DELETE edge row — endpoints and embeddings cascade automatically | Correct ordering; atomicity preserved |
| **Cascade invalidation without audit** | `ON DELETE CASCADE` on endpoint rows triggered by chunk delete | Automatic but loses edge identity for audit; does not satisfy "emit before delete" ordering |
| **Durable endpoint tombstones** | `chunk_id` nullable; deleted chunks replaced with tombstone; `ON DELETE SET NULL` on nullable FK | Preserves audit trail; more complex schema |

**Recommendation for v1**: when a chunk must be deleted, the transaction that
removes the chunk must first delete any `knowledge_edges` whose
`knowledge_edge_endpoints` reference that chunk. The required ordering is:

1. SELECT `qdrant_point_id` values from `knowledge_edge_embeddings` for
   all edges referencing the chunk.
2. INSERT outbox events with those captured point IDs for Qdrant cleanup.
3. DELETE the `knowledge_edges` rows — `ON DELETE CASCADE` removes their
   endpoint and embedding child rows.
4. DELETE the chunk row — no FK violation remains.
5. COMMIT.

The `RESTRICT` FK on `knowledge_edge_endpoints.chunk_id` enforces step 3
before step 4; the FK violation is the signal that the caller omitted
the edge deletion. Qdrant cleanup is driven by the outbox events committed
in step 2, preserving atomicity.

### Prioritise deterministic PostgreSQL traversal over vector retrieval

The BaryGraph PoC's cross-domain bridging claim has no quantitative
validation on enterprise document corpora. The evidence is informal
probe traces on a dictionary.

Wikore's actual differentiators are deterministic and available on day one
with no embedding pipeline:

**Impact analysis at ingest time:**
When a document is re-ingested or superseded, a single PostgreSQL CTE
traversal identifies every affected edge, dependent procedure, wiki page
citing those chunks, and historical chat turn referencing those document
versions. This requires only `knowledge_edges` + `knowledge_edge_endpoints`
with existing FK relationships — no Qdrant collection.

**Governed bridge queries:**
"Which Finance process is affected by this HR policy?" requires joining
edge structure with live EvidenceGate authorization. Flat vector retrieval
cannot do this safely: the connection between chunks must be validated for
both endpoints independently.

**Contradiction and exception detection:**
Two active policy chunks connected by a `contradicts` or `exception_to`
edge surface automatically for governance review — deterministic, auditable,
no LLM call.

**Recommendation:** build deterministic edges and PG traversal first.
The Qdrant edge collection adds semantic similarity search for bridge
queries but is the secondary capability.

### Path vectors from edge vectors — no additional embedding calls

For bounded multi-hop paths (max 2–3 hops), derive a path vector
algebraically from the participating edge vectors:

```text
path_vector = normalize(
    Σᵢ  hop_penalty^i  *  confidence_i  *  v_edge_i
)

where:
  hop_penalty ∈ (0, 1)  — product-configurable, e.g. 0.8
  confidence_i          — edge.confidence of the i-th hop
  v_edge_i              — Qdrant vector of the i-th edge
```

Path vectors are computed at query time, not stored.

### Combined reranker scoring

When automatic intent returns `AllowedChunk`s and `AllowedRelationship`s
together, the reranker needs a unified score. The `geometric_mean of
endpoint chunk scores` approach is not computable: edge search produces
one edge score, not independent endpoint query scores.

Computable placeholder for formula version 1:

```text
chunk_score        = chunk_similarity * (authority_level / 100.0)

relationship_score = edge_similarity
                   * confidence
                   * origin_review_factor
                   * authority_factor
                   * hop_penalty^(path_length - 1)

where:
  edge_similarity   = Qdrant cosine score from edge collection
  authority_factor  = min(authority_source, authority_target) / 100.0
  origin_review_factor:
    deterministic_rule | administrator → 1.0
    parser (accepted)  → 0.9
    llm_proposal       → 0.8
  hop_penalty = 0.8 per additional hop beyond 1
```

Both scores are in [0, 1]. Calibration across candidate kinds requires
a curated test corpus.

### `embedding_model_id` uses the existing `embedding_models` FK

Wikore already has the `embedding_models` table introduced in
`V003__documents.sql` (line 256). `knowledge_edge_embeddings` should
reference it with `embedding_model_id UUID NOT NULL REFERENCES
embedding_models(id)`, consistent with how `document_chunk_vectors`
references it.

The prior recommendation to use a string `embed_model_name` was
incorrect: the registry table already exists. Outbox payloads may
continue carrying the string name alongside the UUID for observability.

### Org-unit capability grants table

`user_capability_grants` and `group_capability_grants` are insufficient
for org-unit subtree assignment. Add a third table:

```sql
CREATE TABLE org_unit_capability_grants (
    company_id   UUID NOT NULL,
    org_unit_id  UUID NOT NULL,
    capability   TEXT NOT NULL,
    applies_to   TEXT NOT NULL DEFAULT 'self_and_descendants'
                     CHECK (applies_to IN ('self_only', 'self_and_descendants')),
    granted_by   UUID NOT NULL,
    reason       TEXT NOT NULL,
    granted_at   TIMESTAMPTZ NOT NULL DEFAULT now(),
    expires_at   TIMESTAMPTZ,
    revoked_at   TIMESTAMPTZ,
    PRIMARY KEY (company_id, org_unit_id, capability),
    FOREIGN KEY (company_id, org_unit_id)
        REFERENCES org_units(company_id, id),
    FOREIGN KEY (company_id, granted_by)
        REFERENCES users(company_id, id)
);
```

Capability resolution at request time uses `org_unit_closure` (already
maintained by the existing membership system) to resolve subtree
membership, consistent with how role membership is resolved today.

### Implementation sequence for the adapted design

1. **PR: rename + type changes**
   - `AllowedCandidate` → `AllowedChunk`; update `RetrievalOrchestrator`,
     `EvidenceGate`, tests.
   - Introduce `AllowedEvidence = variant<AllowedChunk, AllowedRelationship, AllowedPath>`.

2. **PR: V034 migration**
   - `knowledge_edges` with `UNIQUE(company_id, id)`, V1-only edge type CHECK,
     `role` CHECK on `knowledge_edge_endpoints`, missing chunk_id index.
   - `knowledge_edge_endpoints` (chunk-to-chunk only, composite FK).
   - `knowledge_edge_embeddings` (UUID FK to existing `embedding_models`).
   - `CONSTRAINT TRIGGER ... DEFERRABLE INITIALLY DEFERRED` for two-endpoint
     enforcement at COMMIT (see schema section for correct DDL).
   - `ON DELETE RESTRICT` on chunk FK. BEFORE DELETE trigger captures
     `qdrant_point_id` values into outbox AND history in one trigger.
   - `tenant_features`, `user_capability_grants`, `group_capability_grants`,
     `org_unit_capability_grants`.
   - `privileged_access_sessions`, `_approvals`, `_scopes`.
   - Session duration CHECK removed from schema; enforced in application.
   - **Must also update `src/adapters/postgres/error_mapper.cpp` and
     `tests/test_error_mapper.cpp`** in the same PR. V034 introduces 20+
     new constraint names that the pg_constraint introspection CI test
     (`test_error_mapper.cpp:162`) will detect and fail on. Representative
     entries needed in `k_constraint_map`:

     | Constraint name | Suggested Error mapping |
     |---|---|
     | `knowledge_edges_direction_check` | `Error::invalid_input(...)` |
     | `knowledge_edges_confidence_check` | `Error::invalid_input(...)` |
     | `knowledge_edges_origin_check` | `Error::invalid_input(...)` |
     | `knowledge_edges_review_state_check` | `Error::invalid_input(...)` |
     | `knowledge_edges_edge_type_v1_chk` | `Error::invalid_input(...)` |
     | `knowledge_edges_check` (expires_at) | `Error::invalid_input(...)` |
     | `knowledge_edge_endpoints_ordinal_check` | `Error::invalid_input(...)` |
     | `knowledge_edge_endpoints_role_check` | `Error::invalid_input(...)` |
     | `knowledge_edges_history_change_kind_check` | `Error::invalid_input(...)` |
     | `privileged_access_sessions_access_kind_check` | `Error::invalid_input(...)` |
     | `privileged_access_sessions_status_check` | `Error::invalid_input(...)` |
     | `privileged_access_sessions_purpose_check` | `Error::invalid_input(...)` |
     | `privileged_access_approvals_decision_check` | `Error::invalid_input(...)` |
     | `privileged_access_scopes_applies_to_check` | `Error::invalid_input(...)` |
     | `privileged_access_scopes_maximum_sensitivity_check` | `Error::invalid_input(...)` |
     | `org_unit_capability_grants_applies_to_check` | `Error::invalid_input(...)` |
     | `knowledge_edges_created_by_same_company_fk` | allow-list (programming bug) |
     | `knowledge_edges_reviewed_by_same_company_fk` | allow-list (programming bug) |

     And in `k_allowlist`:

     | Constraint name | Justification |
     |---|---|
     | `knowledge_edges_company_id_id_key` | Composite-FK target UNIQUE |
     | `knowledge_edges_history_live_row_id_valid_from_key` | History infrastructure |
     | `knowledge_edges_history_open_uidx` | Partial unique index |
     | `knowledge_edge_endpoints_edge_id_chunk_id_key` | Programming bug if duplicate |
     | `knowledge_edge_embeddings_embedding_model_id_qdrant_point_id_key` | Programming bug |

     The exhaustive list must be produced by running `\d+ knowledge_edges` etc.
     against the migrated schema and cross-checking against the test output.

3. **PR: explicit chunk-to-chunk edge creation**
   - Admin-authored edges of any v1 semantic type where both chunk UUIDs
     are explicitly provided.
   - `derived_from` is admin-authorable (two known chunk IDs); automated
     creation from ingest provenance requires a `source_chunk_id` FK not
     yet in `document_chunks`.
   - Not in v1: `cites` edges from `wiki_page_sources` — wiki pages are not
     stored as `document_chunks`; no wiki-page-chunk endpoint exists.
   - Not in v1: `supersedes` (version→version), `section_parent_of`
     (section→section), `section_contains` (section→chunk),
     `responsible_team` (chunk→org_unit) — all require non-chunk endpoints.
     These remain traversable in native tables until v2 typed endpoints.

4. **PR: edge vector indexing**
   - Admin/scheduler job: embed `kTypeDescriptions` strings, persist to
     type-vector registry keyed by `(formula_version, edge_type)`.
   - `EmbedEdgeWorker` (scheduler): computes edge vectors using formula v1
     with pre-loaded type vectors, writes to Qdrant edge collection.

5. **PR: `RelationshipEvidenceGate` + `RelationshipVectorStorePort`**

6. **PR: retrieval orchestration bridge intent**

7. **PR: `ContextBuilder` accepting `AllowedEvidence`**


## Qdrant representation

A separate relationship collection is preferable initially. It avoids mixing payload schemas and scoring behavior with chunk vectors.

Suggested edge payload:

```cpp
struct EdgePayload {
    static constexpr int kSchemaVersion = 1;

    Uuid company_id;
    Uuid edge_id;
    std::string edge_type;
    std::vector<Uuid> endpoint_chunk_ids;
    float confidence = 0.0f;
    std::int64_t edge_version = 0;
    int formula_version = 1;
    std::string review_state;
    int payload_schema_version = kSchemaVersion;

    // Recall hints only. Never authoritative.
    std::vector<std::string> endpoint_access_scope_ids;
    std::vector<std::string> endpoint_sensitivity_labels;
    std::vector<std::string> endpoint_lifecycle_statuses;
};
```

The safest initial prefilter is tenant plus accepted/active edge state. Combining endpoint access hints in Qdrant is complex because authorization requires **all** endpoints, whereas a simple `MatchAny` filter implements “any endpoint.” PostgreSQL must therefore remain the final and complete decision.

Qdrant payload staleness can reduce recall but must never create authorization. Edge changes should enqueue an outbox resync event similar to current chunk ACL resync.

## Relationship EvidenceGate design

### Required behavior

Given a batch of `EdgeCandidate` values, the gate should:

1. Deduplicate and bound edge IDs.
2. Fetch accepted, non-expired, non-superseded edges for the tenant.
3. Fetch every endpoint for those edges.
4. Resolve endpoint chunks through document versions and documents.
5. Apply current lifecycle and sensitivity rules.
6. Apply the same live resource visibility logic used by the chunk EvidenceGate.
7. Count required endpoints and authorized endpoints per edge.
8. Admit only edges whose counts match and whose state is valid.
9. Hydrate endpoint text only for admitted edges.
10. Reconstruct results in candidate score order.

The SQL should be set based. It must not execute one authorization query per endpoint.

### SQL shape

Illustrative shape:

```sql
WITH candidate_edges AS (...),
edge_records AS (... tenant and edge state ...),
required_endpoints AS (...),
reader_scope AS (...),
visible_documents AS (... existing live grant logic ...),
authorized_endpoints AS (
    SELECT ...
    FROM required_endpoints ep
    JOIN document_chunks dc ...
    JOIN document_versions dv ...
    JOIN documents d ...
    WHERE dc.company_id = $company
      AND dv.lifecycle_status = ANY($allowed_lifecycle)
      AND dv.sensitivity_label = ANY($allowed_sensitivity)
      AND d.id IN (SELECT doc_id FROM visible_documents)
),
admitted_edges AS (
    SELECT edge_id
    FROM required_endpoints
    LEFT JOIN authorized_endpoints USING (edge_id, ordinal)
    GROUP BY edge_id
    HAVING count(*) = count(authorized_endpoints.ordinal)
       AND count(*) = 2
)
SELECT ... hydrated endpoint fields ...
FROM authorized_endpoints
JOIN admitted_edges USING (edge_id);
```

The production query will need to reuse or factor the existing visibility SQL carefully. Copying it into multiple independently maintained strings creates drift risk. Potential solutions:

- a shared SQL fragment generated in one translation unit;
- a PostgreSQL stable function returning visible chunk IDs;
- a generalized gate query operating on evidence bundles;
- a shared repository helper with identical property tests.

The choice must preserve request deadlines and set-based behavior.

### Rejection reasons

For diagnostics, classify without exposing text:

```cpp
enum class EvidenceRejectionReason {
    tenant_mismatch,
    edge_missing,
    edge_not_accepted,
    edge_expired,
    edge_superseded,
    endpoint_missing,
    scope,
    sensitivity,
    lifecycle,
    tombstone,
    malformed_endpoint_set,
};
```

An edge may have multiple reasons. Diagnostics should preserve all relevant reasons for evaluation while presenting a carefully redacted result to production operators.

## Retrieval orchestration

### Fact intent

```text
embed query
  -> resolve reader scope and clearance
  -> chunk Qdrant search
  -> chunk EvidenceGate
  -> chunk reranking
  -> AllowedEvidence
```

### Bridge intent

```text
embed query
  -> resolve reader scope and clearance
  -> edge Qdrant search
  -> RelationshipEvidenceGate
  -> optional bounded path expansion in PostgreSQL
  -> relationship/path reranking
  -> AllowedEvidence
```

### Automatic intent

Automatic mode may search both candidate sources initially. A separate classifier is not required for the first implementation.

```text
fact candidates + bridge candidates
  -> independent authoritative gates
  -> normalize scores by candidate kind
  -> combined reranking
  -> diversity/coverage selection
```

Later, a lightweight intent classifier may reduce unnecessary work, but classification must not make authorization decisions.

### Score normalization

Chunk and edge scores may not be directly comparable, especially if stored in separate collections or generated by different formulas. Combined ranking should explicitly account for:

- vector similarity;
- relationship confidence;
- endpoint authority levels;
- edge origin and review state;
- path length penalty;
- endpoint diversity;
- source freshness;
- direct fact versus inferred bridge coverage.

Do not merge two result lists by treating raw Qdrant scores as a common calibrated scale without evaluation.

## Bounded path traversal

BaryGraph Lite should not begin with arbitrary recursive traversal.

Initial limits:

- maximum two hops by default;
- maximum three hops under an explicit server policy;
- maximum candidate edges per expansion;
- maximum distinct nodes;
- maximum total paths;
- cycle detection by edge and endpoint ID;
- shared request deadline;
- cancellation checks between expansions.

PostgreSQL recursive CTEs may be used for deterministic impact queries, but online answer retrieval should remain tightly bounded and observable.

Path scoring can begin simply:

```text
path_score = geometric_mean(edge_scores)
             * hop_penalty^(hop_count - 1)
             * minimum_endpoint_authority_factor
```

Every formula must be versioned and evaluated. Avoid presenting path scores as probabilities.

## Relationship creation pipeline

### Phase 1: explicit chunk-to-chunk edges (origin = 'administrator')

Start with administrator-authored semantic relationships between two
chunks. These use `origin = 'administrator'` — not `deterministic_rule`,
which is reserved for edges produced by a deterministic extraction rule
with no human decision in the loop.

V1 provides no automated edge creation from existing data because:
- `derived_from` is admin-authorable (admin supplies both chunk UUIDs).
  Automated population from ingest provenance requires a `source_chunk_id`
  FK not yet in `document_chunks` — that is a separate enhancement.
- Wiki-page `cites` requires a wiki-page chunk endpoint that does not exist.
- All other structural relationships have non-chunk endpoints.

Administrator-authored edges require human effort for every edge. The team
should decide before V1 release whether this provides sufficient value
without automated extraction, or whether Phase 2 parser extraction should
be scoped into the initial release.

**Not v1 knowledge_edges** — the following relationships have non-chunk
endpoints and remain in their native tables for v1 impact-analysis queries.
They are not inserted into `knowledge_edges` until typed endpoints land:

| Relationship | Native table / column | v1 impact query |
|---|---|---|
| Wiki page cites document/chunk | `wiki_page_sources` | Join on `document_version_id` or `chunk_id` |
| Section hierarchy | `document_sections.parent_section_id` | Recursive CTE on self-FK |
| Version supersession | `document_versions.superseded_at` (TIMESTAMPTZ, not FK) | Order by `activated_at`, filter `superseded_at IS NULL` for current |
| Chat answer cites chunk | `chat_turns.rag_sources` (JSONB array) | `jsonb_array_elements(rag_sources)` expansion; see V005 + V011 |

These provide useful impact analysis by traversal over native tables and
exercise no authorization path specific to `knowledge_edges`.

### Phase 2: parser and rule-derived edges

Parsers can identify:

- headings indicating exceptions;
- explicit references to policies or controls;
- numbered procedure dependencies;
- defined terms;
- responsibility statements;
- cross-references and hyperlinks.

Rule-derived edges must store the extraction rule and parser version in provenance.

### Phase 3: LLM-proposed edges

LLMs may propose edges but should not directly create accepted relationships.

Recommended lifecycle:

```text
proposed -> reviewed/accepted -> indexed
         -> rejected
accepted -> superseded/expired
```

Store:

- model provider and model name;
- prompt template version;
- endpoint versions;
- extraction timestamp;
- confidence and explanation metadata;
- reviewer identity;
- acceptance timestamp.

Do not store hidden chain-of-thought. Store concise, reviewable provenance and source spans.

## Invalidation and resync

An edge may become stale when:

- an endpoint chunk is deleted or tombstoned;
- an endpoint document version is deprecated or archived;
- sensitivity changes;
- document ownership changes;
- resource grants change;
- edge confidence/type/endpoints change;
- an edge is rejected or superseded;
- the embedding model changes;
- the edge formula version changes;
- **an endpoint chunk's embedding is updated** — e.g., document re-ingest,
  model upgrade, or bug-fix reindex. `indexed_edge_version` tracks the parent
  edge's `edge_version`, not the endpoint chunk vectors; a chunk re-embed
  leaves the edge vector silently stale. Required: the chunk-embed worker
  must `UPDATE knowledge_edges SET edge_version = edge_version + 1 WHERE id
  IN (SELECT edge_id FROM knowledge_edge_endpoints WHERE chunk_id = $1)` and
  enqueue an edge-resync outbox event. The chunk_id index (P1-3) makes this
  efficient.

Required mechanisms:

- edge version increment on semantic changes;
- outbox event for edge payload/vector resync;
- idempotent scheduler worker;
- stale-event protection using edge version or claim token;
- delete/tombstone event removing Qdrant edge points;
- repair job comparing PostgreSQL active edges with Qdrant points;
- metrics for backlog, failures, stale payloads, and orphan points.

Authorization remains safe during lag because PostgreSQL revalidation is authoritative. Lag can still cause recall starvation and must be monitored as a correctness issue.

## Impact analysis

Impact analysis should operate primarily on PostgreSQL relationships and existing citation tables.

Example query products:

- direct dependents of a document version;
- transitive dependents up to a bounded depth;
- active wiki pages citing a deprecated version;
- accepted procedures connected to a changed policy;
- historical chat turns citing affected versions;
- edges that became invalid because one endpoint is gone;
- departments reachable through responsibility/dependency edges.

Results should distinguish:

- direct, deterministic dependency;
- inferred or proposed relationship;
- active versus historical relationship;
- confirmed versus unreviewed impact;
- currently visible versus present-but-redacted counts.

Count disclosure can also leak sensitive graph structure. A caller who cannot see an affected object should not necessarily see that one exists. Impact reports require the same all-endpoint authorization or an explicitly designed aggregate disclosure policy.

## Context construction and answer semantics

Relationship evidence must be represented clearly to the LLM:

```text
[REL R1: implements, confidence=confirmed]
Endpoint A [SRC S1]: HR contractor policy, section “Approval”
Endpoint B [SRC S2]: Finance budget procedure, section “Contractors”
Direction: S2 implements S1
```

Rules:

- endpoint text remains separately cited;
- the relationship itself receives an ID/citation;
- inferred relationships must be labeled as inferred;
- deterministic citations and explicit administrator edges should be distinguished from model proposals;
- the model must not claim a relationship stronger than its type or provenance;
- contradiction edges should present both statements, not silently choose one;
- path answers should expose intermediate steps;
- every final citation must map to allowed evidence used in context.

The prompt builder should budget tokens across endpoint text and relationship metadata. Repeating the same chunk for multiple edges should be deduplicated.

## Diagnostics and evaluation enablers

### Retrieval diagnostics

```cpp
struct CandidateSummary {
    std::string candidate_kind; // chunk or edge
    Uuid candidate_id;
    float score = 0.0f;
    std::vector<EvidenceRejectionReason> rejection_reasons;
};

struct RetrievalDiagnostics {
    std::vector<CandidateSummary> raw_candidates;
    std::vector<CandidateSummary> rejected_by_scope;
    std::vector<CandidateSummary> rejected_by_sensitivity;
    std::vector<CandidateSummary> rejected_by_lifecycle;
    std::vector<CandidateSummary> rejected_by_tombstone;
    std::vector<Uuid> allowed_candidate_ids;
};
```

Production diagnostics must not contain source text or inaccessible endpoint IDs unless the diagnostic caller is independently authorized to inspect them.

The ordinary Qdrant prefilter hides rejected candidates. A true shadow diagnostic therefore needs a separate tenant-only, tightly bounded candidate search followed by classification in PostgreSQL. It must never feed those candidates into answer generation.

### Oracle evaluation

Oracle evaluation is useful for comparing:

- flat chunk retrieval;
- edge retrieval;
- combined retrieval;
- expected bridge discovery;
- authorization rejection behavior;
- impact traversal completeness;
- vector formula variants.

It should exist in a separate test executable or compile-time-gated fixture adapter with:

- fixture-only tenant/database marker;
- no production endpoint;
- no production credentials;
- mock or disabled LLM;
- startup refusal under production configuration;
- deterministic expected edges and paths.

### Evaluation measures

Do not optimize only conventional chunk recall. BaryGraph Lite needs relationship-specific evaluation:

- edge precision at K;
- endpoint authorization precision: must always be 100%;
- bridge discovery success on curated cross-department questions;
- path correctness;
- path explanation completeness;
- contradiction/exception precision;
- impact-analysis precision and recall;
- citation correctness;
- flat-versus-bridge answer quality;
- latency and candidate amplification;
- recall loss during Qdrant resync lag;
- diagnostic rejection-reason accuracy.

## Break-glass is separate

Break-glass is not required to build BaryGraph Lite. It is a temporary production authorization expansion, whereas bridge retrieval changes which authorized evidence shapes are retrieved.

If implemented later, break-glass must:

- require a dedicated permission;
- require a reason;
- have a short TTL;
- remain tenant-bound;
- be explicitly scoped;
- support optional dual approval;
- audit activation, use, query, path, and viewed source;
- fail closed if mandatory audit fails;
- continue through EvidenceGate;
- disable LLM generation by default unless separately approved.

It must not be represented by `is_admin`, `bypass_gate`, or a client-controlled boolean.

## API considerations

Potential request shape after the chat/retrieval API is designed:

```json
{
  "query": "Which Finance process is affected by this HR policy?",
  "retrieval_intent": "bridge",
  "max_relationships": 10,
  "max_path_hops": 2
}
```

The server must cap every value. Diagnostic and oracle modes must not be selectable through this ordinary request.

Potential response metadata:

```json
{
  "answer": "...",
  "citations": [...],
  "relationships": [
    {
      "id": "...",
      "type": "depends_on",
      "source_citation": "S1",
      "target_citation": "S2",
      "origin": "deterministic_rule",
      "confidence": 0.95
    }
  ]
}
```

Internal confidence values may not always belong in end-user responses. The product should distinguish operator/debug metadata from user-facing explanation.

## Audit requirements

Suggested actions:

- `knowledge.edge.proposed`;
- `knowledge.edge.accepted`;
- `knowledge.edge.rejected`;
- `knowledge.edge.superseded`;
- `knowledge.edge.indexed`;
- `knowledge.edge.retrieved`;
- `knowledge.path.retrieved`;
- `knowledge.impact.queried`;
- `retrieval.shadow_diagnostics.used`;
- `retrieval.oracle_eval.executed` in test environments only.

Chat persistence should record the exact edge IDs, endpoint chunk IDs, document version IDs, formula versions, and edge versions used to construct an answer. This allows later impact analysis to identify answers affected by changed relationships.

Audit records must not contain raw prompt or source text by default.

## Performance and operational limits

Relationship retrieval can amplify candidates quickly. Initial hard limits should include:

- maximum Qdrant edge candidates per query;
- maximum candidate endpoints passed to PostgreSQL;
- maximum admitted relationships;
- maximum path hops;
- maximum traversal nodes and edges;
- maximum diagnostic candidates;
- shared request deadline;
- per-tenant concurrency and rate limits;
- maximum endpoint text tokens per relationship;
- maximum repeated endpoint uses in context.

The relationship gate should batch candidates into bounded set-based queries. Avoid N+1 endpoint hydration.

Metrics:

- edge candidates searched;
- edges allowed/rejected by reason;
- gate latency;
- endpoint count per edge;
- path expansion factor;
- Qdrant/PG stale-version mismatch;
- edge resync backlog;
- edge creation/review rate;
- bridge-query latency;
- percentage of answers using relationship evidence;
- relationship citations later invalidated or superseded.

## Testing strategy

### Compile-time/type tests

- `EdgeCandidate` is not convertible to `AllowedRelationship`.
- `RetrievalDiagnostics` is not accepted by `ContextBuilder`.
- `ContextBuilder` accepts only allowed evidence.
- Qdrant payload types do not contain hydrated text.

### Database tests

- Cross-tenant endpoints cannot be inserted.
- Parent edge and endpoint company must match.
- Exactly two endpoints are required in Lite.
- Duplicate endpoints are rejected.
- Invalid edge types, confidence, state, and direction are rejected.
- Actor/reviewer company must match.
- History/supersession remains intact after deletion workflows.

### Authorization property tests

- An edge is allowed if and only if all endpoints are allowed.
- One denied endpoint denies the whole edge.
- Mixed sensitivity uses the strict effective requirement.
- Admin status does not imply restricted clearance.
- Other-tenant candidates are never admitted.
- Deprecated, archived, deleted, or tombstoned endpoints deny the edge.
- Expired grants deny affected endpoints.
- Grant revocation denies an edge immediately at the authoritative gate.
- Stale Qdrant payload can cause recall loss but cannot authorize an edge.
- Paths are allowed if and only if every edge and distinct endpoint is allowed.

### Retrieval tests

- Fact intent searches chunks.
- Bridge intent searches edges.
- Automatic intent combines both without score-scale assumptions.
- Relationship ordering remains stable after gate drops.
- Duplicate endpoints are deduplicated in context.
- Bounded traversal terminates under cycles.
- Candidate and path limits cannot overflow.
- Deadline expiry cancels expansion and returns an error.

### Invalidation tests

- Endpoint lifecycle change enqueues edge resync.
- Endpoint deletion removes or tombstones the edge point.
- Edge review-state change removes proposed/rejected points.
- Formula-version change produces deterministic replacement IDs.
- Stale outbox events cannot overwrite a newer edge version.

### End-to-end tests

- A bridge question retrieves the expected edge and both sources.
- A caller with access to only one source receives neither the edge nor the hidden endpoint.
- Relationship citations map to the exact source versions used.
- Changing a policy produces the expected bounded impact report.
- LLM input contains only allowed relationship evidence.
- Cancellation releases HTTP, Redis, and database resources.

## Recommended implementation sequence

The detailed 7-step PR sequence is in the **"Wikore-adapted design
rationale"** section above. At a higher level:

### Foundation (before closing Iteration 3)

1. `AllowedCandidate` → `AllowedChunk` rename + `AllowedEvidence` variant (PR).
2. V034 migration: edge tables, capability grants, privileged access sessions.
3. Explicit administrator-authored chunk-to-chunk edges (V1 produces no automatically created edges from existing data; every edge requires a human-supplied chunk UUID pair and edge type from the V1 allowed set).
4. Edge vector indexing: admin job persists type vectors; scheduler worker computes and upserts edge vectors.
5. `RelationshipEvidenceGate` + `RelationshipVectorStorePort`.
6. Retrieval orchestration bridge intent.
7. `ContextBuilder` accepting `AllowedEvidence`.

### Complete Iteration 3

8. Full `AskQuestion` composition with `AllowedEvidence`.
9. LLM streaming and cancellation safety.
10. `AnswerFinalizer` with relationship citations.
11. Persist chat turn, audit record, source IDs, edge IDs atomically.
12. Cross-tenant, cancellation, load, and citation-correctness tests.

### Later expansion

13. Deterministic impact-analysis endpoints.
14. Administrator relationship review workflows.
15. Parser/rule extraction pipeline.
16. Reviewed LLM-proposed edges.
17. Typed endpoints (section, document version, org unit) — v2 schema.
18. Three-hop traversal only if two-hop evidence is insufficient.


## Migration and regression strategy

The initial rollout should be additive:

- no change to existing chunk authorization semantics;
- no removal of current Qdrant fields;
- no modification of membership/resource-grant meaning;
- no edge evidence in ordinary answers until the edge gate is proven;
- relationship retrieval behind a server-side feature flag;
- shadow comparison before user-visible activation;
- deterministic edges before model-generated edges;
- independent rollback by disabling edge retrieval while retaining tables.

Suggested rollout states:

```text
disabled
  -> indexing_only
  -> shadow_retrieval
  -> operator_preview
  -> selected_tenants
  -> generally_available
```

The feature flag must control candidate generation, not authorization. EvidenceGate remains mandatory in every enabled state.

## Decisions required before implementation

1. Is the product name `BaryGraph`, `BaryGraph Lite`, or another term?
2. Are first-version endpoints strictly chunks?
3. ~~Which edge types are product-defined for the first release?~~ **Settled:** V1 types are enforced by `knowledge_edges_edge_type_v1_chk`. V2 types (`supersedes`, `section_*`, `responsible_team`) blocked until typed endpoints.
4. Are edge types tenant-extensible?
5. Which deterministic relationships provide the first customer-visible value?
6. Are symmetric edges stored once with canonical endpoint order?
7. Do directed edges use endpoint roles, an explicit direction field, or both?
8. Is `confidence` extraction confidence, relationship truth confidence, or ranking quality?
9. What are `w_ep` and `w_type` for formula version 1, and when should they be revisited for v2?
10. Which vector formula becomes version 1?
11. Which embedding model generates relation-type vectors?
12. Does every accepted edge require human review, or only LLM proposals?
13. What happens to an edge when an endpoint version is superseded?
14. Do impact reports show redacted counts for inaccessible objects?
15. Is two-hop traversal sufficient for the first release?
16. How are chunk and edge scores calibrated for combined reranking?
17. Should relationship citations be visible in the end-user UI?
18. Which audit events are mandatory and which are operational telemetry?
19. Does relationship retrieval belong in the existing collection or a separate Qdrant collection?
20. What is the tenant-level rollout and rollback mechanism?

## Implementation concerns and source-verified notes

The following notes remain valid after reconciliation with the adapted
design sections above. Superseded conclusions have been removed.

### `AllowedCandidate` → `AllowedChunk` rename must precede `ContextBuilder`

The current `RetrievalOrchestrator` output type is
`std::vector<AllowedCandidate>`. The proposed `ContextBuilder` accepts
`std::span<const AllowedEvidence>` where
`AllowedEvidence = variant<AllowedChunk, AllowedRelationship, AllowedPath>`.
If `AllowedChunk ≠ AllowedCandidate`, every call site between the two
needs an implicit conversion that undermines the compile-time safety the
variant is supposed to provide.

The rename touches `RetrievalOrchestrator`, `EvidenceGate`, their tests,
and the single use in `AllowedEvidence`. Do it in one dedicated PR before
`ContextBuilder` is written (see implementation sequence step 1).

### Session duration CHECK belongs in the application layer

`privileged_access_sessions` originally proposed:

```sql
CHECK (expires_at <= starts_at + INTERVAL '24 hours')
```

Some enterprise legal reviews span 48–72 hours. This is a product-policy
constraint, not a schema invariant. Move to the application layer where
it can be tenant-configurable. The schema enforces only `expires_at > starts_at`.

Implement BaryGraph Lite as a relationship-evidence extension to Wikore’s existing permission-safe retrieval architecture, not as a separate graph product and not as an authorization bypass.

The minimum valuable system is:

```text
two-endpoint typed edges in PostgreSQL
  + versioned edge vectors in Qdrant
  + deterministic edge creation
  + all-endpoint RelationshipEvidenceGate
  + fact/bridge retrieval intents
  + AllowedEvidence-only ContextBuilder
  + diagnostics and fixture evaluation
  + bounded impact traversal
```

This is enough to support relationship-aware answers, cross-department bridges, impact analysis, contradictions, exceptions, and meaningful section expansion while preserving Wikore’s defining guarantee: no evidence reaches an answer unless the caller is authorized for all of it under live authoritative state.
