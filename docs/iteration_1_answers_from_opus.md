# Wikore - Iteration 1 Audit (Opus 4.8)

Reviewer: Opus 4.8. Scope: schema V001-V028 and the Iteration 1 C++ (ingest
pipeline, RAG adapters, transaction layer, parser). I read the actual source,
not just the prompt summary - several real files differ from the abridged
prompt (e.g. `document_chunks` has `content`/`content_hash`, not `text`;
`chat_turns.session_id` not `chat_session_id`; `access.cpp` is a stub).

Verdict up front: the schema is unusually disciplined and the ingest write
path is careful. But there are **five issues that will force a fundamental
DB or code change if not decided now**, and they are exactly the "ah shit"
class you want to avoid. They are in section 0. Everything else is normal
review fodder.

---

## 0. Decide BEFORE iteration 2 (rework-class)

These are not bugs you patch later - they change the data model or a core
contract, so the cost grows with every row ingested and every line of the
HTTP layer written on top of the current assumptions.

### A. The `access_scope_ids` denormalization cannot express the grant model the schema advertises  [HIGHEST]

This is the big one. The whole security story is: ingest freezes a chunk's
visibility into `document_chunks.access_scope_ids` (a `UUID[]`), and at query
time the resolver computes the user's `effective_read_orgs` and Qdrant does a
`MatchAny` intersection. For that to be correct, the set written at ingest and
the set computed at query must be provably consistent for every grant shape.

Today `fetch_access_scopes` (`src/ingest/document_repo.cpp:47-81`) does:

```sql
SELECT owner_org_unit_id ...                      -- the owner
UNION
SELECT rg.principal_id FROM resource_grants rg    -- grant principals, VERBATIM
WHERE rg.resource_type = 'document' AND rg.resource_id = $doc
  AND rg.principal_type = 'org_unit' AND rg.permission IN ('read','write','admin')
```

It ignores two things the schema explicitly supports:

1. **`principal_applies_to = 'self_and_descendants'` is not expanded.** A grant
   to "Legal + descendants" injects only `Legal` into the chunk's scope array.
   At query time a member of *Legal-Subteam* has an `effective_read_orgs` set
   that expands **downward** from their own memberships - it contains
   Legal-Subteam and its descendants, never the *ancestor* `Legal`. So the
   intersection misses and the sub-team member cannot see a document the grant
   was meant to share with them.

2. **`resource_type = 'org_unit'` grants are ignored entirely.** The canonical
   example in V002's own header - "Legal (org_unit, self_and_descendants)
   granted read on HR" - is a grant with `resource_type='org_unit'`,
   `resource_id=HR`. A document *owned by* HR (or by an HR sub-team) is never
   matched by the `WHERE resource_type='document' AND resource_id=$doc` query,
   so `Legal` never lands in that document's `access_scope_ids`. Cross-unit
   subtree sharing - a headline feature - is silently non-functional for
   retrieval.

Net effect: only the narrowest grant (document-level, principal `self_only`)
actually works through retrieval. The other shapes parse, store, validate, and
do nothing.

Why it is rework-class: making them work means either
- expanding the principal subtree into every affected chunk's array and
  **re-running a resync over the whole corpus whenever a grant or the org tree
  changes** (the `lr:resync:q` path referenced in V012's comment but never
  built), or
- switching to the typed-prefix "access_tokens" model V002's own comment warns
  you will eventually need.

Either way the meaning of `access_scope_ids` and the resolver change. Doing
that after the corpus is embedded means re-ingesting/re-syncing everything.

**Recommendation:** before any chat/retrieval code, write a one-page set-algebra
spec: for each of the four grant shapes (doc/org-unit x self/self+descendants),
state the exact set that lands in `access_scope_ids` and the exact set
`effective_read_orgs` returns, and show their intersection equals intended
visibility. Build the resync worker in the same iteration that exposes grants.
Until then, document that only document-level `self_only` grants are honored,
so nobody ships a "share with Legal" button that silently leaks nothing (or,
worse, that someone later "fixes" by widening the array and over-grants).

### B. `UnitOfWork::commit()` silently swallows COMMIT failure  [HIGH, correctness]

`src/adapters/postgres/unit_of_work.cpp:12-34`. `commit()` sets
`committed_ = true`, then `co_await CommitAwaiter{...}` whose `await_resume()`
returns `committed_ok` - **and the bool is discarded**. A COMMIT that PG
rejects (serialization failure raised at commit under `40001`, a deferred
constraint, disk-full, connection drop) is reported to the caller as success.

Concrete failure mode in the code you already have: `IngestDocumentVersionUseCase::execute`
flips the row to `processing` *outside* the transaction, does all writes +
`mark_ingest_done` *inside*, then `co_await uow.commit()`. If that commit
fails, the row rolls back to `processing`, no `outbox_events` row exists - but
`execute()` returns `Result<void>{}` (success). The worker marks the job done
and never retries. The document is stuck in `processing` forever, never
embedded, never visible. For `promote_document_version` the user is told the
version was promoted while nothing changed.

**Fix:** `commit()` must return `Result<void>` (or throw `Error`) and set
`committed_ = true` only when `committed_ok` is true; propagate at every call
site. This is small and should land before more use cases are written on top of
the current signature.

### C. Partitioned tables run out of partitions in ~1 year  [HIGH, operational]

`audit_log` (V007) has explicit quarterly partitions only through 2027-Q2;
`usage_events` (V016) monthly through 2027-06. Both have a `_default` catch-all,
so inserts never fail - which is the trap. Once rows land in `*_default`, you
**cannot** `CREATE` the proper partition for that range without detaching the
default, moving its rows, and re-attaching - a locking maintenance operation on
a hot, append-only table. There is no job that creates future partitions
(`wikore_scheduler` is a stub).

**Fix:** add a scheduled "ensure next N partitions exist" job now, plus a
monitor/alert if any row lands in a `*_default` partition. Cheap today,
genuinely painful the first time it bites in production.

### D. `sensitivity_label` is enforced nowhere in retrieval  [HIGH, security-by-iter-2]

V014 is explicit: `restricted` must never appear in generated answers, and
guest vs member sessions differ by allowed labels. But `QdrantFilter`
(`include/wikore/rag/types.hpp`) and `build_search_body`
(`src/rag/retriever.cpp:114-144`) filter on `company_id` + `access_scope_ids` +
`lifecycle_status` only. `ChunkPayload` carries `sensitivity_label`, but no
query path reads it. The day the chat route ships on top of this filter,
confidential/restricted content is retrievable by anyone merely in-scope.

**Fix (iter 2, but design now):** add `sensitivity_label` (a set) to
`QdrantFilter`, emit it in the search body, and define the
session-type -> allowed-labels mapping in one place. Flagging now so the filter
struct gets the field before callers hard-code around its absence.

### E. The Qdrant payload is missing fields the retrieval/rerank design needs  [HIGH, forces re-sync]

`ChunkPayload` (`include/wikore/rag/types.hpp`) lacks `authority_level` and
`owner_org_unit_id`, both named in the V003 payload contract. The reranker
(per PLAN.md) is supposed to weight by `authority_level`; there is nowhere to
put it. Adding payload fields *after* points are written means re-reading PG
and issuing payload-update calls across the whole collection. While
`ChunkPayload` is the only writer/reader and there is no production data, this
is a struct edit; later it is a migration of the vector store.

**Fix:** add `authority_level` (from `documents`) and `owner_org_unit_id` to
`ChunkPayload`, bump `kSchemaVersion`, and make the (not-yet-built) outbox
worker populate them. Also pick one name - the struct uses `document_id`, the
V003 contract comment says `doc_id`; internal-only, so just make them agree and
delete the dead contract text.

---

## 1. Correctness bugs (fix in iteration 1/2)

### F. `error_mapper` constraint map is stale and incomplete  [MEDIUM]

`src/adapters/postgres/error_mapper.cpp:19-65`.

- References constraints that don't exist: `org_unit_self_loop_chk` (V001 has
  no such constraint), `resource_grants_target_chk` (the message "user or
  group, not both" contradicts the org-unit-only V002 schema - leftover from an
  old design), `memberships_expires_after_granted_chk`.
- Misses most real ones: `memberships_exactly_one_principal`,
  `rg_resource_applies_to_valid`, `rg_principal_applies_to_valid`,
  `org_units_root_parent_shape_chk`, the unique indexes on
  `users`/`groups`/`wiki_pages`, and everything from V017/V022/V023/V025/V027.

Unmapped violations fall through to `Error::database_error(...)` -> opaque 500s
for what should be typed 409/400 responses. The comment "Every named constraint
from V001-V015 must appear here" is already false.

**Fix:** add a test that introspects `pg_constraint` + `pg_indexes` and asserts
every CHECK/UNIQUE name is either in the map or in an explicit allow-list, so
the map cannot silently drift as migrations are added.

### G. Re-ingest leaves orphaned chunks/sections when the new layout is smaller  [MEDIUM]

`write_chunks` and `upsert_section` use `ON CONFLICT (document_version_id,
chunk_index)` / `(..., ordinal)` with `DO UPDATE`. If a re-run on the same
`document_version_id` produces *fewer* chunks/sections than a prior attempt
(retry after a chunker-config change between deploys, or a partial earlier
write), the surplus high-index rows are never removed. They keep stale content
and stale `access_scope_ids`, and would be embedded into Qdrant by the worker.

Low probability in the happy path (each content change is a new `version_id`;
ordinary retries reuse the same file and produce the same layout), but it is a
silent landmine.

**Fix:** within the UoW, either `DELETE ... WHERE chunk_index >= :n` (and
`ordinal >= :n`) after the upserts, or do delete-all-for-version then insert.

### H. Idempotency keys include `trace_id`, defeating cross-request dedup  [MEDIUM]

`ingest_document_version.cpp` and `promote_document_version.cpp` build
`idempotency_key` as `...:{trace_id}`. Two genuinely distinct requests for the
same version produce two `outbox_events` rows -> two embed/upsert jobs. They are
harmless (point id is `uuid_v5(chunk+model)` and Qdrant upsert is idempotent),
but wasteful, and the `UNIQUE (company_id, job_type, idempotency_key)` guard now
only catches exact same-request retries - not "the same version got ingested
twice", which is the case you actually want to dedup.

**Fix:** key on `(version_id, embed_model_id)` (ingest) / `(doc_id, version_id,
new_status)` (promote) without `trace_id`; keep `trace_id` in the payload for
tracing.

### I. Ingest has no precondition guard on version state  [MEDIUM-LOW]

`execute` checks `ctx.tenant.company_id == cmd.company_id` (good) but never
checks the version is in `pending`/`processing`. If a stale/duplicate job runs
against a version that is already `active`, `set_ingest_status('processing')`
flips `ingest_status` away from `done`, which violates
`document_versions_active_state_chk` (active requires `ingest_status='done'`).
The job then errors confusingly, and the `fail()` path's
`set_ingest_status('error')` violates the same CHECK again. No corruption, but a
bad failure mode.

**Fix:** make the `processing` transition conditional (`WHERE ingest_status IN
('pending','processing')` or `lifecycle_status='draft'`) and treat a no-op
update as "already processed, skip".

---

## 2. Lower-severity / notes

### J. `uuid_v5` returns an all-zero sentinel on digest failure  [LOW]

`include/wikore/rag/types.hpp`: on `EVP` failure it returns
`00000000-0000-0000-0000-000000000000`. If it ever fires, every affected chunk
shares one point id and all but one silently vanish from Qdrant. Contrast
`sha256_hex` in `document_repo.cpp`, which returns `""` and the caller errors
out - that one is correct. Make `uuid_v5` fallible the same way (or assert).

### K. Promote re-activation corrupts the as-of interval  [LOW]

V010 `promote_document_version` sets `superseded_at = NULL` and keeps the old
`activated_at` via `COALESCE`. Re-promoting a version that went
active -> deprecated -> active erases the gap: the as-of history now reads
"active continuously from the original `activated_at`", which the V003
point-in-time queries will silently believe. Edge case (manual rollback), but
the as-of feature is a selling point. Either forbid deprecated -> active without
an explicit `reactivate_document_version()`, or reset `activated_at` on
re-promotion.

### L. CRLF is not normalized in the parser  [LOW]

`PlainTextParser` splits via `std::getline` on `\n`; CRLF files leave a trailing
`\r` on every heading and body line, polluting `heading_path` and chunk text.
Strip trailing `\r`.

### M. `full_text` is dead weight in the current ingest path  [INFO]

The parser always pushes at least one section, so the chunker's
`doc.sections.empty()` branch (the only consumer of `full_text`) never runs.
Fine if `full_text` is intended for later use; otherwise drop it to avoid
doubling memory on large documents.

### N. Float precision in vector serialization  [INFO]

`retriever.cpp` serializes embeddings with `{:.8g}`. For 768-dim cosine this is
almost certainly fine, but it is a lossy choice made implicitly; note it so a
future "why do C++ and Python scores differ in the 4th decimal" hunt is short.

---

## 3. What is genuinely well done (so it doesn't get "refactored" away)

- **Composite-FK tenancy** (`FOREIGN KEY (company_id, x) REFERENCES t(company_id, id)`)
  is applied consistently and is the strongest part of the design - cross-tenant
  references are impossible at the DB, not just in code. Keep it; do not let a
  future "simplify the FKs" PR weaken it.
- **Deprecate-first / activate-second** ordering in both promote functions is
  correct against the non-deferrable partial unique index. The reasoning in the
  V010 comment is sound.
- **Append-only and immutability triggers** (audit_log, usage_events, outbox,
  tombstones, prompt_templates, shared_chat snapshot) are correct and a good
  defence-in-depth layer beyond the role-level GRANTs.
- **Fail-closed retrieval**: empty `access_scope_ids` -> `MatchAny []` -> zero
  results, mirrored in `NullVectorStore`. This is the right default and the test
  pins it.
- **UTF-8 decoder** rejects overlong encodings and surrogates; the parser also
  strips Unicode tag chars and hidden HTML - good prompt-injection hygiene.
- **`write_chunks` digest-failure handling** correctly aborts on an empty hash
  (the model `uuid_v5` should copy).
- **UoW destructor** rolls back uncommitted transactions and logs - good safety
  net (once finding B makes the success path honest too).
- **`move_org_unit`** closure surgery and the session-GUC-gated parent guard are
  correct and the concurrency contract is documented honestly.

---

## 4. Suggested ordering

1. **B** (commit swallows failure) - small, foundational, do first.
2. **A** (access_scope_ids spec + resync plan) - decide before any retrieval
   code; it's the only finding that gets more expensive per ingested row.
3. **E**, **D** (payload fields + sensitivity in the filter) - struct/contract
   edits that are cheap now, a vector re-sync later.
4. **C** (partition job) - before go-live, not before iter 2.
5. **F, G, H, I** - during iteration 2 as the write/HTTP paths solidify.
6. **J-N** - opportunistic.

Nothing here blocks continuing to build. A, B, D, E are the ones that, left
unaddressed, become "we have to re-ingest the corpus / change the resolver /
re-sync every vector" later. Lock those decisions in now.
