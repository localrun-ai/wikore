# Redis Key Reference

All keys are prefixed with `lr:` (localrun namespace).

## Auth

| Key | Value | TTL | Notes |
|-----|-------|-----|-------|
| `lr:jwt:{jti}` | `"1"` | until JWT exp | Revoked JTIs (logout / forced revoke). Presence = revoked. |
| `lr:api_key:{key_hash}` | JSON Identity | 5 min | API key fast-path; invalidate on revoke |

## Access resolution

The critical path for RAG: resolving `access_scope_ids` (which org_unit_ids a user can
read when querying a given org_unit) must be fast and consistent.

| Key | Value | TTL | Notes |
|-----|-------|-----|-------|
| `lr:eff:{company_id}:{user_id}:{org_unit_id}` | JSON array of UUID strings | see below | Resolved access_scope_ids for this user+scope; drives Qdrant filter |
| `lr:eff:keys:user:{company_id}:{user_id}` | Redis Set of `lr:eff:*` key names | 30 min (refreshed on every SADD) | Reverse index: tracks all cached eff keys for this user in this company |
| `lr:eff:keys:company:{company_id}` | Redis Set of `lr:eff:*` key names | 30 min (refreshed on every SADD) | Reverse index: tracks all cached eff keys for this company (for company-wide invalidation) |
| `lr:user:{user_id}` | JSON Identity record | 5 min | Profile + is_admin; invalidate on user update |
| `lr:tree:{company_id}:{org_unit_id}` | JSON subtree | 2 min | Admin UI tree; invalidate on any mutation under this node |

### Reverse-index sets: why and how

Production Redis guidance bans KEYS and SCAN with wildcards (O(keyspace) operation
that blocks the event loop). Instead, each `lr:eff` cache write also registers the
full key name into two reverse-index Sets so invalidation can be O(actual cached
entries) rather than O(total keyspace).

**On every `lr:eff` cache write:**
```
SET    lr:eff:{cid}:{uid}:{oid} <value> EX <ttl>
SADD   lr:eff:keys:user:{cid}:{uid}    "lr:eff:{cid}:{uid}:{oid}"
EXPIRE lr:eff:keys:user:{cid}:{uid}    1800
SADD   lr:eff:keys:company:{cid}       "lr:eff:{cid}:{uid}:{oid}"
EXPIRE lr:eff:keys:company:{cid}       1800
```
Set TTL for reverse-index keys must be longer than the eff TTL (30 min vs 5 min)
so the Set outlives its members. The `EXPIRE` MUST be reissued on every `SADD`:
`SADD` does not refresh TTL, so without the explicit `EXPIRE` an active user's
reverse-index Set would die at the 30-min mark even though their eff entries
keep getting rewritten every 5 min - subsequent invalidations would then see
an empty Set and silently skip the user's stale eff entries until those
entries expire naturally. Implementations SHOULD batch the five commands
above into a single `MULTI/EXEC` or Lua script for atomicity (otherwise a
crash between `SADD` and `EXPIRE` leaves a Set with no TTL, leaking memory).
Stale members (keys already expired) are harmless: DEL on a non-existent
key is a no-op.

**On invalidation (e.g., membership change for user U in company C):**
```
SMEMBERS lr:eff:keys:user:{C}:{U}   -> [key1, key2, ...]
DEL key1 key2 ...
DEL lr:eff:keys:user:{C}:{U}
```

**On company-wide invalidation (e.g., org_unit delete):**
```
SMEMBERS lr:eff:keys:company:{C}    -> [key1, key2, ...]
DEL key1 key2 ...
DEL lr:eff:keys:company:{C}
```
After the DEL loop, also remove the per-user reverse-index keys for affected users
(or let them expire naturally - they are self-consistent since the eff keys are gone).

### lr:eff TTL policy (grant expiry awareness)

`resource_grants.expires_at` creates a correctness hazard: if an effective scope entry
is cached with a fixed 5-minute TTL but a contributing grant expires within that window,
the user retains access after the grant has lapsed.

**Resolution: Option B - clamp TTL to the earliest contributing expiry.**

As of V011, both `resource_grants` and `memberships` carry `expires_at`. The
access resolver must consider both when computing the cache TTL.

C++ resolver rule:
```
effective cache TTL = min(default 5 min,
                          MIN(resource_grants.expires_at) for contributing grants,
                          MIN(memberships.expires_at) for contributing memberships)
```

Steps:
1. Collect `MIN(expires_at)` across all `resource_grants` AND `memberships` rows
   that contribute to this user's effective scope (both may be NULL per row).
2. If `min_expiry IS NULL` (no time-limited entries): use default TTL = 5 min.
3. If `min_expiry - now() < 0`: already lapsed; do not cache (or set TTL = 1s).
4. If `min_expiry - now() < 5 min`: set TTL = `CEIL(min_expiry - now())`.
5. Otherwise: use default TTL = 5 min.

Worst-case stale window: 1 clock-tick between the resolver reading `MIN(expires_at)`
and writing the Redis TTL. Acceptable for enterprise use.

## Rate limiting

| Key | Value | TTL | Notes |
|-----|-------|-----|-------|
| `lr:rl:chat:{user_id}` | counter | 60 s | Sliding window; max N chat requests/min per user |
| `lr:rl:ingest:{org_unit_id}` | counter | 60 s | Ingest rate per org_unit |

## LLM guardrails (per-tenant, Lua-atomic; see `rag::LlmGate`)

| Key | Value | TTL | Notes |
|-----|-------|-----|-------|
| `lr:llm:sem:{company_id}` | ZSET of lease tokens, scored by expiry (server-clock ms) | key: 2x lease | Per-tenant concurrency semaphore, capped by `LLM_CONCURRENCY`. A lease token is added on `sem_acquire` scored `now + max_hold + margin` and removed on `sem_release`; expired leases (crashed holders) are pruned on the next acquire, so a slot is never leaked. Now is read from `redis.call('TIME')` so all app nodes share one clock. |
| `lr:llm:rate:{company_id}` | HASH `{t: tokens, ts: last_ms}` | ~burst/rate | Per-tenant token-bucket rate limiter (`LLM_RATE_PER_SEC` refill, `LLM_RATE_BURST` capacity). Refill + take is one atomic `token_bucket_take` EVAL, also clocked by `redis.call('TIME')`. |

## Ingest queue

| Key | Value | TTL | Notes |
|-----|-------|-----|-------|
| `lr:ingest:q:{org_unit_id}` | List of doc_id | - | Serializes ingests per org_unit; LPOP = claim work |
| `lr:ingest:lock:{doc_id}` | worker_id | 10 min | Distributed lock; prevents duplicate chunk processing |

## Qdrant payload resync queue

When document visibility changes (resource_grant create/revoke, document owner_org_unit
change, org-unit moves affecting inherited grants), affected chunk payloads
(access_scope_ids) must be updated in Qdrant. Membership and group member changes do
NOT trigger payload resyncs; see the invalidation table below. Changes are enqueued
here for background processing.

| Key | Value | TTL | Notes |
|-----|-------|-----|-------|
| `lr:resync:q` | List of JSON {company_id, org_unit_id, reason} | - | Background worker pops and recomputes chunk scopes |
| `lr:resync:lock:{company_id}:{org_unit_id}` | worker_id | 5 min | Prevent parallel resync for same scope |

## MCP tool cache (read-only tools only)

| Key | Value | TTL | Notes |
|-----|-------|-----|-------|
| `lr:tool:{tool_name}:{sha256(args)}` | JSON result | 5 min | Cache Jira search, Confluence page, etc. Write tools NEVER cached. |
| `lr:oauth:{integration_id}` | access_token | until exp | OAuth token cache; refresh before expiry |

## Invalidation rules

### Mental model: two independent axes

`access_scope_ids` in a Qdrant chunk payload answers: **"which org_unit IDs grant
visibility to this chunk?"** This is a property of the document/grant configuration,
not of the user population.

`lr:eff:{company_id}:{user_id}:{org_unit_id}` answers: **"which org_unit IDs does
this user belong to when querying from this scope?"** This is a property of membership
and group composition.

At query time the filter is: `access_scope_ids intersects user.resolved_scopes`.

The two axes change independently:

- **Membership/group changes** shift user.resolved_scopes. They never alter
  access_scope_ids stored in Qdrant. Only `lr:eff` keys need invalidation; Qdrant
  payloads are untouched.

- **Grant/document/org-structure changes** shift which org_units appear in
  access_scope_ids. Qdrant payload resync is required; `lr:eff` invalidation
  is also needed for any user whose scope resolution depended on those grants.

Treating membership changes as triggers for Qdrant resync is wrong: adding a single
user to HR would queue a rewrite of every HR document payload, making routine admin
operations O(chunks_in_scope) expensive. The correct cost is O(1) Redis key deletes.

### Invalidation table

"SMEMBERS+DEL user" means: SMEMBERS `lr:eff:keys:user:{C}:{U}` -> DEL those keys -> DEL the Set.
"SMEMBERS+DEL company" means: SMEMBERS `lr:eff:keys:company:{C}` -> DEL those keys -> DEL the Set.

| Event | Redis keys to delete | Qdrant resync? |
|-------|----------------------|----------------|
| Membership add/remove (principal=user) | SMEMBERS+DEL user for affected user | No - access_scope_ids unchanged |
| Membership add/remove (principal=group) | SMEMBERS+DEL user for every member of that group | No - access_scope_ids unchanged |
| Group member add/remove | SMEMBERS+DEL user for the affected user | No - access_scope_ids unchanged |
| Resource grant create/revoke | SMEMBERS+DEL user for each principal in affected org_unit's member list | Yes - access_scope_ids of in-scope chunks change; enqueue `lr:resync:q` |
| Document owner_org_unit change | SMEMBERS+DEL user for members of old and new org_unit | Yes - access_scope_ids recomputed for all chunks of this document |
| Document lifecycle change (activate/archive) | none | Yes - lifecycle_status field in Qdrant payload |
| Org_unit create | `lr:tree:{company_id}:{parent_id}` | No - no chunks yet |
| Org_unit move | `lr:tree:{company_id}:{old_parent}`, `lr:tree:{company_id}:{new_parent}`, SMEMBERS+DEL user for all users in moved subtree | Yes - descendants-grants now expand over a different subtree; enqueue `lr:resync:q` for moved subtree |
| Org_unit delete | SMEMBERS+DEL company, then DEL all `lr:tree:{company_id}:*` keys (tracked separately or scanned at low frequency) | Yes - chunks owned by deleted unit removed from Qdrant |
| User update | `lr:user:{user_id}` | No |
| API key revoke | `lr:api_key:{key_hash}` | No |
