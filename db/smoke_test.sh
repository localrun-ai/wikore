#!/usr/bin/env bash
# db/smoke_test.sh - Schema smoke test suite
#
# Spins up a throwaway Postgres 17 container, loads all migrations, runs 25
# behavioral checks, then tears down.  Exit code 0 = all passed.
#
# Usage: ./db/smoke_test.sh

set -euo pipefail
cd "$(dirname "$0")/.."

CONTAINER=wikore_smoke_$$
PG_IMAGE=postgres:17
PASS=0
FAIL=0

pass() { echo "PASS $1: $2"; PASS=$((PASS+1)); }
fail() { echo "FAIL $1: $2"; FAIL=$((FAIL+1)); }

# --------------------------------------------------------------------------
# UUIDs (all chars must be 0-9 or a-f)
# --------------------------------------------------------------------------
CO_ACME='ac000001-0000-0000-0000-000000000000'  # company Acme
CO_BETA='be000001-0000-0000-0000-000000000000'  # company Beta
CO_TEMP='ca110000-0000-0000-0000-000000000000'  # throwaway company
U_ALICE='a1100001-0000-0000-0000-000000000000'  # user Alice (Acme)
U_BOB='b0b00001-0000-0000-0000-000000000000'    # user Bob (Beta)
OU_HR='0a100001-0000-0000-0000-000000000000'    # HR department (Acme child)
DOC='d0c00001-0000-0000-0000-000000000000'      # document
VER1='7e100001-0000-0000-0000-000000000000'     # version 1 (active)
VER2='7e100002-0000-0000-0000-000000000000'     # version 2 (second active - should fail)
VER3='7e100003-0000-0000-0000-000000000000'     # version 3 (different version for FK test)
CHK1='c0100001-0000-0000-0000-000000000000'     # chunk in VER3
WIKI='b100a001-0000-0000-0000-000000000000'     # wiki page
WVER='b100a002-0000-0000-0000-000000000000'     # wiki page version 1
VER4='7e100004-0000-0000-0000-000000000000'     # version 4 (for promotion tests)
OU_ENG='0a100002-0000-0000-0000-000000000000'   # Engineering department (Acme child, for move test)
OUTBOX1='0b0c0001-0000-0000-0000-000000000001'  # outbox event 1
SEC1='5ec10001-0000-0000-0000-000000000000'     # document_section (for K5 test)

# --------------------------------------------------------------------------
# Start container and load schema
# --------------------------------------------------------------------------
echo "-- Starting $PG_IMAGE container ($CONTAINER)..."
docker run --rm -e POSTGRES_PASSWORD=test -d --name "$CONTAINER" "$PG_IMAGE" > /dev/null

# Robust readiness gate. The official postgres image runs a TEMPORARY server to
# perform initdb and then restarts the real one. A naive "can I connect?" probe
# can succeed against that temporary server and then race its shutdown: the
# socket disappears and the next psql fails with "No such file or directory".
# So wait for the entrypoint's init-complete marker first (the boundary after
# which only the real server runs), THEN for that server to accept connections.
echo "-- Waiting for Postgres to finish init..."
for _ in $(seq 1 120); do
  docker logs "$CONTAINER" 2>&1 | grep -q "init process complete" && break
  sleep 1
done
echo "-- Waiting for Postgres to accept connections..."
ready=0
for _ in $(seq 1 120); do
  if docker exec "$CONTAINER" pg_isready -U postgres -q 2>/dev/null \
     && docker exec "$CONTAINER" psql -U postgres -tAc 'SELECT 1' >/dev/null 2>&1; then
    ready=1; break
  fi
  sleep 1
done
if [ "$ready" -ne 1 ]; then
  echo "-- Postgres did not become ready; recent container logs:"
  docker logs --tail 40 "$CONTAINER" 2>&1 || true
  docker rm -f "$CONTAINER" > /dev/null 2>&1 || true
  exit 1
fi

psql() { docker exec "$CONTAINER" psql -U postgres -At "$@"; }
sql()  { psql -c "$1"; }

echo "-- Provisioning runtime roles..."
docker exec -i "$CONTAINER" psql -U postgres -v ON_ERROR_STOP=1 \
  < db/provision_roles.sql
# A second run must converge cleanly; deployments may reapply provisioning.
docker exec -i "$CONTAINER" psql -U postgres -v ON_ERROR_STOP=1 \
  < db/provision_roles.sql

echo "-- Loading migrations..."
# Migrations are auto-discovered (sorted lexicographically = sequential
# numeric order) so adding a new V0NN__*.sql file is purely additive
# from a PR-merge standpoint - no explicit cat-list edit needed.
ls db/migrations/V*.sql | sort | xargs cat \
  | docker exec -i "$CONTAINER" psql -U postgres -v ON_ERROR_STOP=1 -q
echo "-- Migrations loaded."
echo ""

# --------------------------------------------------------------------------
# Fixtures shared across tests
# --------------------------------------------------------------------------
sql "INSERT INTO companies (id, name, slug) VALUES
    ('$CO_ACME', 'Acme', 'acme'),
    ('$CO_BETA', 'Beta', 'beta');" > /dev/null

sql "INSERT INTO users (id, company_id, external_sub, email) VALUES
    ('$U_ALICE', '$CO_ACME', 'sub1', 'alice@acme.com'),
    ('$U_BOB',   '$CO_BETA', 'sub2', 'bob@beta.com');" > /dev/null

ACME_ROOT=$(sql "SELECT id FROM org_units WHERE company_id='$CO_ACME' AND type='root';")
BETA_ROOT=$(sql "SELECT id FROM org_units WHERE company_id='$CO_BETA' AND type='root';")

# --------------------------------------------------------------------------
# 1. Insert company -> root org_unit auto-created
# --------------------------------------------------------------------------
COUNT=$(sql "SELECT COUNT(*) FROM org_units WHERE company_id='$CO_ACME' AND type='root';")
[ "$COUNT" -eq 1 ] \
  && pass 1 "root org_unit auto-created on company insert" \
  || fail 1 "root org_unit missing (got $COUNT)"

# --------------------------------------------------------------------------
# 2. Root closure self-entry exists at depth 0
# --------------------------------------------------------------------------
COUNT=$(sql "SELECT COUNT(*) FROM org_unit_closure
             WHERE company_id='$CO_ACME'
               AND ancestor_id='$ACME_ROOT'
               AND descendant_id='$ACME_ROOT'
               AND depth=0;")
[ "$COUNT" -eq 1 ] \
  && pass 2 "root closure self-entry at depth 0 exists" \
  || fail 2 "root closure self-entry missing (got $COUNT)"

# --------------------------------------------------------------------------
# 3. Direct root delete is blocked by trigger
# --------------------------------------------------------------------------
ERR=$(sql "DELETE FROM org_units WHERE id='$ACME_ROOT';" 2>&1 || true)
echo "$ERR" | grep -q "Cannot delete root org_unit" \
  && pass 3 "direct root delete blocked by trigger" \
  || fail 3 "direct root delete was not blocked (got: $ERR)"

# --------------------------------------------------------------------------
# 4. Company delete cascades (root trigger allows when company row is gone)
# --------------------------------------------------------------------------
sql "INSERT INTO companies (id, name, slug)
     VALUES ('$CO_TEMP', 'Temp', 'temp');" > /dev/null
ERR=$(sql "DELETE FROM companies WHERE id='$CO_TEMP';" 2>&1 || true)
echo "$ERR" | grep -qi "error\|exception" \
  && fail 4 "company cascade delete failed: $ERR" \
  || pass 4 "company delete cascades (root trigger allows when company is gone)"

# --------------------------------------------------------------------------
# 5. Non-root child insert creates full closure chain
# --------------------------------------------------------------------------
sql "INSERT INTO org_units (id, company_id, parent_id, type, slug, name)
     VALUES ('$OU_HR', '$CO_ACME', '$ACME_ROOT', 'department', 'hr', 'HR');" > /dev/null

# Expect 2 rows: self (depth 0) + root ancestor (depth 1)
ROWS=$(sql "SELECT COUNT(*) FROM org_unit_closure
            WHERE company_id='$CO_ACME' AND descendant_id='$OU_HR';")
[ "$ROWS" -eq 2 ] \
  && pass 5 "child closure chain correct ($ROWS ancestor rows: self + root)" \
  || fail 5 "expected 2 ancestor rows for child, got $ROWS"

# --------------------------------------------------------------------------
# 6. Direct parent_id update is blocked
# --------------------------------------------------------------------------
ERR=$(sql "UPDATE org_units SET parent_id='$OU_HR'
           WHERE id='$OU_HR';" 2>&1 || true)
echo "$ERR" | grep -q "parent_id cannot be updated" \
  && pass 6 "direct parent_id update blocked by trigger" \
  || fail 6 "parent_id update was not blocked (got: $ERR)"

# --------------------------------------------------------------------------
# 7. Cross-company parent insert fails (composite FK)
# --------------------------------------------------------------------------
ERR=$(sql "INSERT INTO org_units (company_id, parent_id, type, slug, name)
           VALUES ('$CO_ACME', '$BETA_ROOT', 'department', 'cross', 'Cross');" 2>&1 || true)
echo "$ERR" | grep -qi "foreign key\|violates" \
  && pass 7 "cross-company parent insert rejected by composite FK" \
  || fail 7 "cross-company parent insert was not blocked (got: $ERR)"

# --------------------------------------------------------------------------
# 8. Membership granted_by from wrong company is rejected
# --------------------------------------------------------------------------
ERR=$(sql "INSERT INTO memberships (company_id, user_id, org_unit_id, role, granted_by)
           VALUES ('$CO_ACME', '$U_ALICE', '$ACME_ROOT', 'viewer', '$U_BOB');" 2>&1 || true)
echo "$ERR" | grep -q "does not belong to company" \
  && pass 8 "membership with cross-company granted_by rejected" \
  || fail 8 "cross-company granted_by was not blocked (got: $ERR)"

# --------------------------------------------------------------------------
# 9. At most one active document version per document
# --------------------------------------------------------------------------
sql "INSERT INTO documents (id, company_id, owner_org_unit_id, filename)
     VALUES ('$DOC', '$CO_ACME', '$ACME_ROOT', 'policy.pdf');" > /dev/null

sql "INSERT INTO document_versions
       (id, company_id, document_id, version_no, source_hash,
        ingest_status, completed_at, activated_at, chunk_count, lifecycle_status)
     VALUES ('$VER1', '$CO_ACME', '$DOC', 1, 'h1',
             'done', now(), now(), 5, 'active');" > /dev/null

ERR=$(sql "INSERT INTO document_versions
             (id, company_id, document_id, version_no, source_hash,
              ingest_status, completed_at, activated_at, chunk_count, lifecycle_status)
           VALUES ('$VER2', '$CO_ACME', '$DOC', 2, 'h2',
                   'done', now(), now(), 3, 'active');" 2>&1 || true)
echo "$ERR" | grep -qi "unique\|duplicate" \
  && pass 9 "second active version rejected by partial unique index" \
  || fail 9 "second active version was not blocked (got: $ERR)"

# --------------------------------------------------------------------------
# 10. wiki_page_sources chunk must belong to the cited version
# --------------------------------------------------------------------------
sql "UPDATE document_versions
     SET lifecycle_status='deprecated', superseded_at=now()
     WHERE id='$VER1';" > /dev/null

sql "INSERT INTO document_versions
       (id, company_id, document_id, version_no, source_hash,
        ingest_status, completed_at, activated_at, chunk_count, lifecycle_status)
     VALUES ('$VER3', '$CO_ACME', '$DOC', 3, 'h3',
             'done', now(), now(), 1, 'active');" > /dev/null

sql "INSERT INTO document_chunks
       (id, company_id, document_version_id, chunk_index, content, content_hash)
     VALUES ('$CHK1', '$CO_ACME', '$VER3', 0, 'text', 'ch');" > /dev/null

sql "INSERT INTO wiki_pages (id, company_id, org_unit_id, slug, title)
     VALUES ('$WIKI', '$CO_ACME', '$ACME_ROOT', 'policy', 'Policy');" > /dev/null

sql "INSERT INTO wiki_page_versions
       (id, company_id, wiki_page_id, version_no, content, content_hash, lifecycle_status)
     VALUES ('$WVER', '$CO_ACME', '$WIKI', 1, 'Content v1', 'h_wiki1', 'active');" > /dev/null

# CHK1 belongs to VER3 but we cite it under VER1 - must fail
ERR=$(sql "INSERT INTO wiki_page_sources
             (company_id, wiki_page_version_id, document_id, document_version_id, chunk_id)
           VALUES ('$CO_ACME', '$WVER', '$DOC', '$VER1', '$CHK1');" 2>&1 || true)
echo "$ERR" | grep -qi "foreign key\|violates" \
  && pass 10 "wiki_page_sources chunk from wrong version rejected by FK" \
  || fail 10 "chunk from wrong version was not blocked (got: $ERR)"

# --------------------------------------------------------------------------
# 11-15. promote_document_version() tests
# --------------------------------------------------------------------------
# Set up a second 'done' document version (VER4) for promotion tests.
# VER3 is currently the active version.
sql "INSERT INTO document_versions
       (id, company_id, document_id, version_no, source_hash,
        ingest_status, completed_at, chunk_count, lifecycle_status)
     VALUES ('$VER4', '$CO_ACME', '$DOC', 4, 'h4',
             'done', now(), 2, 'draft');" > /dev/null

# --------------------------------------------------------------------------
# 11. Direct activate-first update fails (unique violation: VER3 already active)
# --------------------------------------------------------------------------
ERR=$(sql "UPDATE document_versions
           SET lifecycle_status='active', activated_at=now(), superseded_at=NULL
           WHERE id='$VER4';" 2>&1 || true)
echo "$ERR" | grep -qi "unique\|duplicate" \
  && pass 11 "direct activate without deprecating old version rejected by partial unique index" \
  || fail 11 "direct activate-first was not blocked (got: $ERR)"

# --------------------------------------------------------------------------
# 12. promote_document_version() succeeds
# --------------------------------------------------------------------------
ERR=$(sql "SELECT promote_document_version('$CO_ACME', '$DOC', '$VER4');" 2>&1 || true)
echo "$ERR" | grep -qi "error\|exception" \
  && fail 12 "promote_document_version() raised an error: $ERR" \
  || pass 12 "promote_document_version() completed without error"

# --------------------------------------------------------------------------
# 13. Exactly one active version remains after promotion
# --------------------------------------------------------------------------
COUNT=$(sql "SELECT COUNT(*) FROM document_versions
             WHERE company_id='$CO_ACME' AND document_id='$DOC'
               AND lifecycle_status='active';")
[ "$COUNT" -eq 1 ] \
  && pass 13 "exactly one active version after promotion (got $COUNT)" \
  || fail 13 "expected 1 active version after promotion, got $COUNT"

# --------------------------------------------------------------------------
# 14. Previously active version (VER3) now has superseded_at set
# --------------------------------------------------------------------------
COUNT=$(sql "SELECT COUNT(*) FROM document_versions
             WHERE id='$VER3'
               AND lifecycle_status='deprecated'
               AND superseded_at IS NOT NULL;")
[ "$COUNT" -eq 1 ] \
  && pass 14 "old active version deprecated with superseded_at set" \
  || fail 14 "old active version not correctly deprecated (got $COUNT)"

# --------------------------------------------------------------------------
# 15. New active version (VER4) has activated_at set and superseded_at NULL
# --------------------------------------------------------------------------
COUNT=$(sql "SELECT COUNT(*) FROM document_versions
             WHERE id='$VER4'
               AND lifecycle_status='active'
               AND activated_at IS NOT NULL
               AND superseded_at IS NULL;")
[ "$COUNT" -eq 1 ] \
  && pass 15 "new active version has activated_at set and superseded_at NULL" \
  || fail 15 "new active version state incorrect (got $COUNT)"

# --------------------------------------------------------------------------
# 16-17. move_org_unit() tests
# --------------------------------------------------------------------------
# Create OU_ENG as a sibling of OU_HR (both directly under ACME_ROOT).
# Then move OU_HR under OU_ENG and verify closure is correct.
sql "INSERT INTO org_units (id, company_id, parent_id, type, slug, name)
     VALUES ('$OU_ENG', '$CO_ACME', '$ACME_ROOT', 'department', 'eng', 'Engineering');" > /dev/null

# --------------------------------------------------------------------------
# 16. move_org_unit() completes without error
# --------------------------------------------------------------------------
ERR=$(sql "SELECT move_org_unit('$CO_ACME', '$OU_HR', '$OU_ENG');" 2>&1 || true)
echo "$ERR" | grep -qi "error\|exception" \
  && fail 16 "move_org_unit() raised an error: $ERR" \
  || pass 16 "move_org_unit() completed without error"

# --------------------------------------------------------------------------
# 17. Moved org_unit has correct closure rows (self + OU_ENG + ACME_ROOT)
# --------------------------------------------------------------------------
ROWS=$(sql "SELECT COUNT(*) FROM org_unit_closure
            WHERE company_id='$CO_ACME' AND descendant_id='$OU_HR';")
[ "$ROWS" -eq 3 ] \
  && pass 17 "moved org_unit closure correct: 3 ancestor rows (self + OU_ENG + root)" \
  || fail 17 "expected 3 ancestor rows after move, got $ROWS"

# --------------------------------------------------------------------------
# 18. move_org_unit() no-op (already direct child) returns without error,
#     leaves closure unchanged (Opus V2-N6 early-exit guard)
# --------------------------------------------------------------------------
ERR=$(sql "SELECT move_org_unit('$CO_ACME', '$OU_HR', '$OU_ENG');" 2>&1 || true)
ROWS=$(sql "SELECT COUNT(*) FROM org_unit_closure
            WHERE company_id='$CO_ACME' AND descendant_id='$OU_HR';")
if echo "$ERR" | grep -qi "error\|exception"; then
    fail 18 "no-op move_org_unit() raised: $ERR"
elif [ "$ROWS" -eq 3 ]; then
    pass 18 "no-op move_org_unit() returns clean and leaves closure intact"
else
    fail 18 "no-op move corrupted closure (expected 3 rows, got $ROWS)"
fi

# --------------------------------------------------------------------------
# 19. promote_document_version() refuses to revive an archived version
#     (Opus V2-N5 archived-is-terminal guard)
# --------------------------------------------------------------------------
# VER3 was deprecated by test 12's promotion. Archive it, then try to promote.
sql "UPDATE document_versions SET lifecycle_status='archived'
     WHERE id='$VER3';" > /dev/null
ERR=$(sql "SELECT promote_document_version('$CO_ACME', '$DOC', '$VER3');" 2>&1 || true)
echo "$ERR" | grep -qi "not promotable\|archived" \
  && pass 19 "promote_document_version() rejects archived version" \
  || fail 19 "archived version was promoted (expected rejection, got: $ERR)"

# --------------------------------------------------------------------------
# 20-22. reactivate_user() tests (V013, Opus V2-N3)
# --------------------------------------------------------------------------
# Soft-deactivate Alice, then revive her via reactivate_user() using her
# original (issuer, sub). The function must clear deactivated_at and
# refresh PII fields rather than fail on the unique constraint.
sql "UPDATE users SET deactivated_at=now() WHERE id='$U_ALICE';" > /dev/null

# --------------------------------------------------------------------------
# 20. reactivate_user() revives Alice, returns her existing id, clears deactivated_at
# --------------------------------------------------------------------------
RID=$(sql "SELECT reactivate_user('$CO_ACME', 'local', 'sub1',
                                  'alice+new@acme.com', 'Alice Reactivated', NULL);")
[ "$RID" = "$U_ALICE" ] \
  && pass 20 "reactivate_user() returned Alice's existing id" \
  || fail 20 "reactivate_user() returned wrong id (expected $U_ALICE, got $RID)"

# --------------------------------------------------------------------------
# 21. After reactivation, deactivated_at is NULL and PII fields refreshed
# --------------------------------------------------------------------------
ROW=$(sql "SELECT deactivated_at IS NULL AND email='alice+new@acme.com'
                  AND display_name='Alice Reactivated'
           FROM users WHERE id='$U_ALICE';")
[ "$ROW" = "t" ] \
  && pass 21 "reactivate_user() cleared deactivated_at and refreshed PII" \
  || fail 21 "reactivate_user() did not properly revive user (got $ROW)"

# --------------------------------------------------------------------------
# 22. reactivate_user() for unknown (issuer, sub) inserts a new user row
# --------------------------------------------------------------------------
NID=$(sql "SELECT reactivate_user('$CO_ACME', 'local', 'sub_new',
                                  'newcomer@acme.com', 'New Comer', NULL);")
COUNT=$(sql "SELECT COUNT(*) FROM users
             WHERE id='$NID' AND company_id='$CO_ACME'
               AND external_sub='sub_new' AND deactivated_at IS NULL;")
[ "$COUNT" -eq 1 ] \
  && pass 22 "reactivate_user() inserts new user when (issuer,sub) unknown" \
  || fail 22 "reactivate_user() did not create new user row (got count $COUNT)"

# --------------------------------------------------------------------------
# 23. K1: sensitivity_label defaults to 'internal' on new document_version
# --------------------------------------------------------------------------
# VER4 was created without an explicit sensitivity_label; DEFAULT applies.
VAL=$(sql "SELECT sensitivity_label FROM document_versions WHERE id='$VER4';")
[ "$VAL" = "internal" ] \
  && pass 23 "document_versions.sensitivity_label defaults to 'internal'" \
  || fail 23 "unexpected sensitivity_label default (got '$VAL')"

# --------------------------------------------------------------------------
# 24. K5: document_section inserts; chunk section_id FK accepted
# --------------------------------------------------------------------------
sql "INSERT INTO document_sections
       (id, company_id, document_version_id, ordinal, heading, heading_path, depth)
     VALUES ('$SEC1', '$CO_ACME', '$VER4', 1, '§1 Introduction', '{\"§1 Introduction\"}', 0);" > /dev/null

# Assign the section to CHK1 (which belongs to VER3, not VER4 -- simple FK
# only checks existence, not version consistency; that's ingest-enforced).
ERR=$(sql "UPDATE document_chunks SET section_id='$SEC1' WHERE id='$CHK1';" 2>&1 || true)
echo "$ERR" | grep -qi "error\|exception\|violates" \
  && fail 24 "section_id FK update failed: $ERR" \
  || pass 24 "document_chunks.section_id accepted valid section reference"

# --------------------------------------------------------------------------
# 25. K2: documents.deleted_at + document_chunk_tombstones insert work
# --------------------------------------------------------------------------
sql "UPDATE documents SET deleted_at=now() WHERE id='$DOC';" > /dev/null
sql "INSERT INTO document_chunk_tombstones
       (company_id, chunk_id, document_version_id, content_hash, reason)
     VALUES ('$CO_ACME', '$CHK1', '$VER3', 'sha256_of_removed_content', 'gdpr_erasure');" > /dev/null

COUNT=$(sql "SELECT COUNT(*) FROM document_chunk_tombstones
             WHERE company_id='$CO_ACME' AND chunk_id='$CHK1';")
[ "$COUNT" -eq 1 ] \
  && pass 25 "document_chunk_tombstone inserted and queryable" \
  || fail 25 "document_chunk_tombstone not found (got count $COUNT)"

# --------------------------------------------------------------------------
# 26. V015: outbox_events table exists and accepts an insert
# --------------------------------------------------------------------------
sql "INSERT INTO outbox_events
       (id, company_id, aggregate_id, job_type, payload, idempotency_key)
     VALUES ('$OUTBOX1', '$CO_ACME', '$DOC', 'qdrant_resync_version_lifecycle',
             '{\"document_id\": \"$DOC\"}', 'smoke:promote:$DOC:$VER4');" > /dev/null

COUNT=$(sql "SELECT COUNT(*) FROM outbox_events WHERE id='$OUTBOX1';")
[ "$COUNT" -eq 1 ] \
  && pass 26 "outbox_events insert accepted" \
  || fail 26 "outbox_events insert failed (count=$COUNT)"

# --------------------------------------------------------------------------
# 27. V015: duplicate idempotency key is rejected
# --------------------------------------------------------------------------
ERR=$(sql "INSERT INTO outbox_events
             (company_id, aggregate_id, job_type, payload, idempotency_key)
           VALUES ('$CO_ACME', '$DOC', 'qdrant_resync_version_lifecycle',
                   '{}', 'smoke:promote:$DOC:$VER4');" 2>&1 || true)
echo "$ERR" | grep -qi "unique\|duplicate" \
  && pass 27 "duplicate idempotency key rejected by unique constraint" \
  || fail 27 "duplicate idempotency key was not rejected: $ERR"

# --------------------------------------------------------------------------
# 28. V015: ON CONFLICT DO NOTHING silently ignores duplicate
# --------------------------------------------------------------------------
sql "INSERT INTO outbox_events
       (company_id, aggregate_id, job_type, payload, idempotency_key)
     VALUES ('$CO_ACME', '$DOC', 'qdrant_resync_version_lifecycle',
             '{}', 'smoke:promote:$DOC:$VER4')
     ON CONFLICT (company_id, job_type, idempotency_key) DO NOTHING;" > /dev/null

COUNT=$(sql "SELECT COUNT(*) FROM outbox_events
             WHERE company_id='$CO_ACME' AND idempotency_key='smoke:promote:$DOC:$VER4';")
[ "$COUNT" -eq 1 ] \
  && pass 28 "ON CONFLICT DO NOTHING: still exactly 1 row after duplicate insert" \
  || fail 28 "unexpected row count after ON CONFLICT DO NOTHING (got $COUNT)"

# --------------------------------------------------------------------------
# 29. V016: usage_events insert lands in the right monthly partition
# --------------------------------------------------------------------------
sql "INSERT INTO usage_events
       (company_id, user_id, event_type, model_name, tokens_in, tokens_out,
        latency_ms, cost_micros, detail)
     VALUES ('$CO_ACME', '$U_ALICE', 'llm_chat', 'qwen3-8b',
             1200, 340, 850, 47500,
             '{\"chat_turn_id\":\"00000000-0000-0000-0000-000000000abc\"}'::jsonb);" > /dev/null

COUNT=$(sql "SELECT COUNT(*) FROM usage_events
             WHERE company_id='$CO_ACME' AND model_name='qwen3-8b';")
[ "$COUNT" -eq 1 ] \
  && pass 29 "usage_events insert accepted and queryable by tenant" \
  || fail 29 "usage_events insert not found (count=$COUNT)"

# --------------------------------------------------------------------------
# 30. V016: usage_events is append-only (UPDATE and DELETE rejected)
# --------------------------------------------------------------------------
ERR_UPD=$(sql "UPDATE usage_events SET cost_micros=99999 WHERE company_id='$CO_ACME';" 2>&1 || true)
ERR_DEL=$(sql "DELETE FROM usage_events WHERE company_id='$CO_ACME';" 2>&1 || true)
echo "$ERR_UPD$ERR_DEL" | grep -qi "append-only" \
  && pass 30 "usage_events UPDATE and DELETE rejected (append-only)" \
  || fail 30 "append-only enforcement missing: upd='$ERR_UPD' del='$ERR_DEL'"

# --------------------------------------------------------------------------
# 31. V017: prompt_templates insert + chat_turn references it via composite FK
# --------------------------------------------------------------------------
PT1='b00b0001-0000-0000-0000-000000000000'   # prompt template 1 (Acme)
SES1='5e550001-0000-0000-0000-000000000000'  # chat session
TRN1='7e550001-0000-0000-0000-000000000000'  # chat turn

sql "INSERT INTO prompt_templates
       (id, company_id, name, content, content_hash, created_by)
     VALUES ('$PT1', '$CO_ACME', 'default_chat',
             'You are Wikore. Cite sources by §.',
             'sha256_of_v1_content', '$U_ALICE');" > /dev/null

sql "INSERT INTO chat_sessions (id, company_id, org_unit_id, user_id, title)
     VALUES ('$SES1', '$CO_ACME', '$ACME_ROOT', '$U_ALICE', 'smoke chat');" > /dev/null

sql "INSERT INTO chat_turns
       (id, company_id, session_id, question, prompt_template_id)
     VALUES ('$TRN1', '$CO_ACME', '$SES1', 'What is our PTO policy?', '$PT1');" > /dev/null

VAL=$(sql "SELECT prompt_template_id FROM chat_turns WHERE id='$TRN1';")
[ "$VAL" = "$PT1" ] \
  && pass 31 "chat_turn pinned to prompt_template via composite FK" \
  || fail 31 "chat_turn.prompt_template_id mismatch (got '$VAL')"

# --------------------------------------------------------------------------
# 32. V017: prompt_templates content/name/hash are immutable
# --------------------------------------------------------------------------
ERR_CON=$(sql "UPDATE prompt_templates SET content='different' WHERE id='$PT1';" 2>&1 || true)
ERR_NAM=$(sql "UPDATE prompt_templates SET name='renamed'     WHERE id='$PT1';" 2>&1 || true)
ERR_HSH=$(sql "UPDATE prompt_templates SET content_hash='x'   WHERE id='$PT1';" 2>&1 || true)
# description should still be mutable
sql "UPDATE prompt_templates SET description='clarified purpose' WHERE id='$PT1';" > /dev/null
DESC=$(sql "SELECT description FROM prompt_templates WHERE id='$PT1';")

echo "$ERR_CON$ERR_NAM$ERR_HSH" | grep -qi "immutable" && [ "$DESC" = "clarified purpose" ] \
  && pass 32 "prompt_template content/name/hash immutable; description mutable" \
  || fail 32 "immutability invariants wrong: con='$ERR_CON' nam='$ERR_NAM' hsh='$ERR_HSH' desc='$DESC'"

# --------------------------------------------------------------------------
# 33. V017: cross-company prompt_template_id rejected by composite FK
# --------------------------------------------------------------------------
PT_BETA='b00b0002-0000-0000-0000-000000000000'
sql "INSERT INTO prompt_templates (id, company_id, name, content, content_hash)
     VALUES ('$PT_BETA', '$CO_BETA', 'default_chat', 'Beta prompt', 'sha256_beta');" > /dev/null

# Try to pin an Acme chat_turn to Beta's prompt template -- must fail.
TRN_BAD='7e550002-0000-0000-0000-000000000000'
ERR=$(sql "INSERT INTO chat_turns
             (id, company_id, session_id, question, prompt_template_id)
           VALUES ('$TRN_BAD', '$CO_ACME', '$SES1', 'cross-tenant?', '$PT_BETA');" 2>&1 || true)
echo "$ERR" | grep -qi "foreign key\|violates" \
  && pass 33 "cross-company prompt_template_id rejected by composite FK" \
  || fail 33 "cross-company prompt_template_id was not rejected: $ERR"

# --------------------------------------------------------------------------
# 34. V018: chat_turn_feedback insert; re-vote UPDATEs the existing row
# --------------------------------------------------------------------------
# TRN1/SES1 already exist from test 31; reuse them for feedback tests.

# First vote: +1
sql "INSERT INTO chat_turn_feedback (company_id, chat_turn_id, user_id, signal)
     VALUES ('$CO_ACME', '$TRN1', '$U_ALICE', 1);" > /dev/null

# Re-vote: same user same turn -> must UPSERT, not raise a unique violation.
sql "INSERT INTO chat_turn_feedback (company_id, chat_turn_id, user_id, signal, reason)
     VALUES ('$CO_ACME', '$TRN1', '$U_ALICE', -1, 'cited wrong section')
     ON CONFLICT (chat_turn_id, user_id) DO UPDATE
       SET signal = EXCLUDED.signal,
           reason = EXCLUDED.reason,
           updated_at = now();" > /dev/null

ROW=$(sql "SELECT signal::text || '|' || COALESCE(reason,'') FROM chat_turn_feedback
           WHERE chat_turn_id='$TRN1' AND user_id='$U_ALICE';")
COUNT=$(sql "SELECT COUNT(*) FROM chat_turn_feedback WHERE chat_turn_id='$TRN1';")
[ "$COUNT" -eq 1 ] && [ "$ROW" = "-1|cited wrong section" ] \
  && pass 34 "chat_turn_feedback UPSERT replaces vote (count=$COUNT, row=$ROW)" \
  || fail 34 "feedback UPSERT wrong (count=$COUNT, row='$ROW')"

# --------------------------------------------------------------------------
# 35. V018: cross-company feedback rejected by composite FK
# --------------------------------------------------------------------------
ERR=$(sql "INSERT INTO chat_turn_feedback (company_id, chat_turn_id, user_id, signal)
           VALUES ('$CO_BETA', '$TRN1', '$U_BOB', 1);" 2>&1 || true)
echo "$ERR" | grep -qi "foreign key\|violates" \
  && pass 35 "cross-company chat_turn_feedback rejected by composite FK" \
  || fail 35 "cross-company feedback was not rejected: $ERR"

# --------------------------------------------------------------------------
# 36. V018: chunk_quality_signals insert; cascades on chunk delete
# --------------------------------------------------------------------------
sql "INSERT INTO chunk_quality_signals
       (company_id, chunk_id, positive_count, negative_count, last_signal_at)
     VALUES ('$CO_ACME', '$CHK1', 3, 1, now());" > /dev/null

# Delete the underlying chunk via version cascade and confirm signals follow.
sql "DELETE FROM document_versions WHERE id='$VER3';" > /dev/null
COUNT=$(sql "SELECT COUNT(*) FROM chunk_quality_signals WHERE chunk_id='$CHK1';")
[ "$COUNT" -eq 0 ] \
  && pass 36 "chunk_quality_signals cascades on chunk delete (rows=0)" \
  || fail 36 "chunk_quality_signals did not cascade (rows=$COUNT)"

# --------------------------------------------------------------------------
# Per-migration tests (auto-discovered from db/smoke_tests/V*.sh)
#
# Each schema PR drops its test file under db/smoke_tests/V0NN__name.sh and
# this loop sources them in lexicographic (numeric) order. Sourcing means
# each file has access to the helpers (sql, psql, pass, fail) and fixtures
# (CO_ACME, U_ALICE, ACME_ROOT, ...) defined above. Per-PR tests should use
# distinct UUIDs and tag IDs by migration number (e.g., "V019.1") to avoid
# cross-file collisions.
# --------------------------------------------------------------------------
for f in db/smoke_tests/V*.sh; do
    [ -f "$f" ] && source "$f"
done

# --------------------------------------------------------------------------
# Summary
# --------------------------------------------------------------------------
echo ""
docker rm -f "$CONTAINER" > /dev/null
TOTAL=$((PASS+FAIL))
echo "-- Results: $PASS/$TOTAL passed"
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
