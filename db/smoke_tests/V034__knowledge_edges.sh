# Per-migration smoke tests for V034 (BaryGraph Lite knowledge edges).

V34_DOC='6e6cd0c0-0000-0000-0000-000000000001'
V34_VER='6e6c7e70-0000-0000-0000-000000000001'
V34_CHK1='6e6cc010-0000-0000-0000-000000000001'
V34_CHK2='6e6cc010-0000-0000-0000-000000000002'
V34_CHK3='6e6cc010-0000-0000-0000-000000000003'
V34_MODEL='6e6cd010-0000-0000-0000-000000000001'
EDGE1='6e6c0000-0000-0000-0000-000000000001'
EDGE2='6e6c0000-0000-0000-0000-000000000002'
EDGE3='6e6c0000-0000-0000-0000-000000000003'
EDGE_DEL='6e6c0000-0000-0000-0000-0000000000d1'
EDGE_ORPHAN='6e6c0000-0000-0000-0000-00000000bad0'
POINT1='6e6c0000-0000-0000-0000-0000000000aa'

# Fixtures (fresh, isolated from earlier smoke tests).
sql "INSERT INTO documents (id, company_id, owner_org_unit_id, filename, title, mime_type)
     VALUES ('$V34_DOC', '$CO_ACME', '$ACME_ROOT', 'v34.txt', 'V034', 'text/plain');" > /dev/null

sql "INSERT INTO document_versions
       (id, company_id, document_id, version_no, source_hash,
        ingest_status, completed_at, activated_at, chunk_count, lifecycle_status)
     VALUES ('$V34_VER', '$CO_ACME', '$V34_DOC', 1, 'v34-h1',
             'done', now(), now(), 3, 'active');" > /dev/null

sql "INSERT INTO document_chunks
       (id, company_id, document_version_id, chunk_index, content, content_hash)
     VALUES ('$V34_CHK1', '$CO_ACME', '$V34_VER', 0, 'first',  'h1'),
            ('$V34_CHK2', '$CO_ACME', '$V34_VER', 1, 'second', 'h2'),
            ('$V34_CHK3', '$CO_ACME', '$V34_VER', 2, 'third',  'h3');" > /dev/null

# embedding_models is a global registry (no company_id column).
sql "INSERT INTO embedding_models (id, name, qdrant_collection, dimension)
     VALUES ('$V34_MODEL', 'v34-test-model', 'v34_test_collection', 768);" > /dev/null

# V034.1: valid two-endpoint edge accepted at COMMIT
sql "BEGIN;
     INSERT INTO knowledge_edges
       (id, company_id, edge_type, direction, confidence, origin, created_by)
     VALUES ('$EDGE1', '$CO_ACME', 'implements', 'directed', 0.9,
             'administrator', '$U_ALICE');
     INSERT INTO knowledge_edge_endpoints (company_id, edge_id, ordinal, chunk_id, role)
     VALUES ('$CO_ACME', '$EDGE1', 0, '$V34_CHK1', 'source'),
            ('$CO_ACME', '$EDGE1', 1, '$V34_CHK2', 'target');
     COMMIT;" > /dev/null
COUNT=$(sql "SELECT count(*) FROM knowledge_edges WHERE id='$EDGE1';")
[ "$COUNT" = "1" ] \
  && pass "V034.1" "valid two-endpoint edge accepted at COMMIT" \
  || fail "V034.1" "valid edge rejected (count=$COUNT)"

# V034.2: initial history captured, lifecycle timestamps present
COUNT=$(sql "SELECT count(*) FROM knowledge_edges_history
             WHERE live_row_id='$EDGE1' AND change_kind='insert'
               AND edge_created_at IS NOT NULL;")
[ "$COUNT" = "1" ] \
  && pass "V034.2" "initial history captured with lifecycle timestamps" \
  || fail "V034.2" "insert history missing (count=$COUNT)"

# V034.3: history endpoint snapshot captured both chunk IDs
COUNT=$(sql "SELECT count(*) FROM knowledge_edges_history
             WHERE live_row_id='$EDGE1'
               AND endpoint_0_chunk_id='$V34_CHK1'
               AND endpoint_1_chunk_id='$V34_CHK2';")
[ "$COUNT" = "1" ] \
  && pass "V034.3" "history endpoint snapshot correct" \
  || fail "V034.3" "endpoint snapshot missing (count=$COUNT)"

# V034.4: zero-endpoint edge rejected at COMMIT (parent-table trigger)
ERR=$(sql "BEGIN;
     INSERT INTO knowledge_edges
       (id, company_id, edge_type, direction, confidence, origin)
     VALUES ('$EDGE_ORPHAN', '$CO_ACME', 'implements', 'directed', 0.9, 'administrator');
     COMMIT;" 2>&1 || true)
echo "$ERR" | grep -qi "exactly two endpoints\|check_violation" \
  && pass "V034.4" "zero-endpoint edge rejected at COMMIT" \
  || fail "V034.4" "zero-endpoint edge accepted: $ERR"

# V034.5: one-endpoint edge rejected at COMMIT
ERR=$(sql "BEGIN;
     INSERT INTO knowledge_edges
       (id, company_id, edge_type, direction, confidence, origin)
     VALUES ('$EDGE_ORPHAN', '$CO_ACME', 'implements', 'directed', 0.9, 'administrator');
     INSERT INTO knowledge_edge_endpoints (company_id, edge_id, ordinal, chunk_id, role)
     VALUES ('$CO_ACME', '$EDGE_ORPHAN', 0, '$V34_CHK1', 'source');
     COMMIT;" 2>&1 || true)
echo "$ERR" | grep -qi "exactly two endpoints\|check_violation" \
  && pass "V034.5" "one-endpoint edge rejected at COMMIT" \
  || fail "V034.5" "one-endpoint edge accepted: $ERR"

# V034.6: reversed insertion order accepted (history deferred to COMMIT)
sql "BEGIN;
     INSERT INTO knowledge_edges
       (id, company_id, edge_type, direction, confidence, origin)
     VALUES ('$EDGE2', '$CO_ACME', 'depends_on', 'directed', 0.8, 'administrator');
     INSERT INTO knowledge_edge_endpoints (company_id, edge_id, ordinal, chunk_id, role)
     VALUES ('$CO_ACME', '$EDGE2', 1, '$V34_CHK2', 'target'),
            ('$CO_ACME', '$EDGE2', 0, '$V34_CHK1', 'source');
     COMMIT;" > /dev/null
COUNT=$(sql "SELECT count(*) FROM knowledge_edges_history
             WHERE live_row_id='$EDGE2' AND change_kind='insert';")
[ "$COUNT" = "1" ] \
  && pass "V034.6" "reversed insertion order accepted (history deferred)" \
  || fail "V034.6" "reversed order rejected (count=$COUNT)"

# V034.7: AFTER UPDATE trigger writes history automatically
sql "UPDATE knowledge_edges SET confidence = 0.5, edge_version = edge_version + 1
     WHERE id='$EDGE1';" > /dev/null
COUNT=$(sql "SELECT count(*) FROM knowledge_edges_history
             WHERE live_row_id='$EDGE1' AND change_kind='update';")
[ "$COUNT" = "1" ] \
  && pass "V034.7" "AFTER UPDATE trigger wrote history automatically" \
  || fail "V034.7" "UPDATE history missing (count=$COUNT)"

# V034.8: exactly one open interval after UPDATE
OPEN=$(sql "SELECT count(*) FROM knowledge_edges_history
             WHERE live_row_id='$EDGE1' AND valid_until IS NULL;")
[ "$OPEN" = "1" ] \
  && pass "V034.8" "exactly one open interval after UPDATE" \
  || fail "V034.8" "open interval count wrong (got $OPEN)"

# V034.9: endpoint UPDATE rejected (immutable)
ERR=$(sql "UPDATE knowledge_edge_endpoints
           SET chunk_id='$V34_CHK3'
           WHERE edge_id='$EDGE1' AND ordinal=0;" 2>&1 || true)
echo "$ERR" | grep -qi "immutable\|insufficient_privilege" \
  && pass "V034.9" "endpoint UPDATE rejected (immutable)" \
  || fail "V034.9" "endpoint UPDATE accepted: $ERR"

# V034.10: endpoint DELETE rejected while parent edge exists (P1-1 fix)
ERR=$(sql "DELETE FROM knowledge_edge_endpoints
           WHERE edge_id='$EDGE1' AND ordinal=0;" 2>&1 || true)
echo "$ERR" | grep -qi "no_orphan_delete\|insufficient_privilege\|cannot be deleted" \
  && pass "V034.10" "endpoint DELETE rejected while parent edge exists" \
  || fail "V034.10" "orphan endpoint DELETE accepted: $ERR"

# V034.11: endpoint DELETE+INSERT swap bypass BLOCKED (P1-1 regression test)
# Attempt to swap endpoints via delete+insert within one transaction.
# Must fail because DELETE is blocked while parent exists.
ERR=$(sql "BEGIN;
     DELETE FROM knowledge_edge_endpoints WHERE edge_id='$EDGE1';
     INSERT INTO knowledge_edge_endpoints (company_id, edge_id, ordinal, chunk_id, role)
     VALUES ('$CO_ACME', '$EDGE1', 0, '$V34_CHK3', 'source'),
            ('$CO_ACME', '$EDGE1', 1, '$V34_CHK2', 'target');
     COMMIT;" 2>&1 || true)
echo "$ERR" | grep -qi "no_orphan_delete\|cannot be deleted" \
  && pass "V034.11" "endpoint DELETE+INSERT bypass blocked (P1-1)" \
  || fail "V034.11" "endpoint swap bypass accepted: $ERR"

# V034.12: edge UUID reuse rejected (P1-2 regression test)
# Delete an edge, then try to re-insert with the same UUID.
sql "BEGIN;
     INSERT INTO knowledge_edges
       (id, company_id, edge_type, direction, confidence, origin)
     VALUES ('$EDGE_DEL', '$CO_ACME', 'implements', 'directed', 0.9, 'administrator');
     INSERT INTO knowledge_edge_endpoints (company_id, edge_id, ordinal, chunk_id, role)
     VALUES ('$CO_ACME', '$EDGE_DEL', 0, '$V34_CHK1', 'source'),
            ('$CO_ACME', '$EDGE_DEL', 1, '$V34_CHK2', 'target');
     COMMIT;" > /dev/null
sql "DELETE FROM knowledge_edges WHERE id='$EDGE_DEL';" > /dev/null

ERR=$(sql "INSERT INTO knowledge_edges
             (id, company_id, edge_type, direction, confidence, origin)
           VALUES ('$EDGE_DEL', '$CO_ACME', 'implements', 'directed', 0.9, 'administrator');" 2>&1 || true)
echo "$ERR" | grep -qi "no_uuid_reuse\|prior history" \
  && pass "V034.12" "edge UUID reuse rejected (P1-2)" \
  || fail "V034.12" "UUID reuse accepted: $ERR"

# V034.13: V2-only edge type rejected by V1 CHECK
ERR=$(sql "INSERT INTO knowledge_edges
             (id, company_id, edge_type, direction, confidence, origin)
           VALUES (gen_random_uuid(), '$CO_ACME', 'supersedes',
                   'directed', 0.9, 'administrator');" 2>&1 || true)
echo "$ERR" | grep -qi "edge_type_v1_chk\|check constraint" \
  && pass "V034.13" "V2-only edge_type rejected by V1 CHECK" \
  || fail "V034.13" "V2 edge_type accepted: $ERR"

# V034.14: composite FK rejects cross-tenant created_by
ERR=$(sql "INSERT INTO knowledge_edges
             (id, company_id, edge_type, direction, confidence, origin, created_by)
           VALUES (gen_random_uuid(), '$CO_ACME', 'implements', 'directed', 0.9,
                   'administrator', '$U_BOB');" 2>&1 || true)
echo "$ERR" | grep -qi "same_company_fk\|foreign key" \
  && pass "V034.14" "composite FK rejects cross-tenant created_by (race-free)" \
  || fail "V034.14" "cross-tenant actor accepted: $ERR"

# V034.15: DELETE emits qdrant_delete_edge_points outbox event with point ID
sql "INSERT INTO knowledge_edge_embeddings
       (company_id, edge_id, embedding_model_id, qdrant_point_id,
        formula_version, indexed_edge_version)
     VALUES ('$CO_ACME', '$EDGE1', '$V34_MODEL', '$POINT1', 1, 1);" > /dev/null

sql "DELETE FROM knowledge_edges WHERE id='$EDGE1';" > /dev/null
COUNT=$(sql "SELECT count(*) FROM outbox_events
             WHERE aggregate_id='$EDGE1' AND job_type='qdrant_delete_edge_points';")
[ "$COUNT" = "1" ] \
  && pass "V034.15" "DELETE emits qdrant_delete_edge_points outbox event" \
  || fail "V034.15" "outbox event missing (count=$COUNT)"

# V034.16: outbox payload contains the exact qdrant_point_id
HAS_POINT=$(sql "SELECT count(*) FROM outbox_events
                 WHERE aggregate_id='$EDGE1'
                   AND job_type='qdrant_delete_edge_points'
                   AND payload->'qdrant_point_ids' @> to_jsonb(ARRAY['$POINT1'::uuid]);")
[ "$HAS_POINT" = "1" ] \
  && pass "V034.16" "outbox payload contains the correct qdrant_point_id" \
  || fail "V034.16" "outbox payload missing point_id (got $HAS_POINT)"

# V034.17: history has delete marker (change_kind='delete')
COUNT=$(sql "SELECT count(*) FROM knowledge_edges_history
             WHERE live_row_id='$EDGE1' AND change_kind='delete';")
[ "$COUNT" = "1" ] \
  && pass "V034.17" "history has delete marker after DELETE" \
  || fail "V034.17" "delete marker missing (count=$COUNT)"

# V034.18 (P2-4): snapshot function is not callable by public
# The function should be present but EXECUTE revoked from PUBLIC.
HAS_EXEC=$(sql "SELECT has_function_privilege(current_user,
                     'knowledge_edges_snapshot_internal(uuid, text)',
                     'EXECUTE');")
# In smoke test we run as postgres superuser, which bypasses REVOKE.
# So instead verify the REVOKE grant was applied:
NO_PUBLIC=$(sql "SELECT NOT EXISTS (
    SELECT 1 FROM information_schema.role_routine_grants
    WHERE routine_name='knowledge_edges_snapshot_internal'
      AND grantee='PUBLIC'
);")
[ "$NO_PUBLIC" = "t" ] \
  && pass "V034.18" "snapshot function has no PUBLIC EXECUTE grant" \
  || fail "V034.18" "PUBLIC can execute snapshot function"

# V034.19: partial unique index rejects duplicate open interval
ERR=$(sql "INSERT INTO knowledge_edges_history
             (live_row_id, edge_id, company_id, edge_type, direction,
              confidence, origin, review_state, provenance,
              formula_version, edge_version,
              edge_created_at,
              endpoint_0_chunk_id, endpoint_1_chunk_id,
              endpoint_0_role, endpoint_1_role,
              change_kind, valid_from, valid_until)
           VALUES ('$EDGE2', '$EDGE2', '$CO_ACME', 'depends_on', 'directed',
                   0.8, 'administrator', 'accepted', '{}',
                   1, 1, now(),
                   '$V34_CHK1', '$V34_CHK2', 'source', 'target',
                   'update', clock_timestamp(), NULL);" 2>&1 || true)
echo "$ERR" | grep -qi "knowledge_edges_history_open_uidx\|unique\|duplicate" \
  && pass "V034.19" "partial unique index rejects duplicate open interval" \
  || fail "V034.19" "duplicate open interval accepted: $ERR"

# V034.20: interval order CHECK
ERR=$(sql "INSERT INTO knowledge_edges_history
             (live_row_id, edge_id, company_id, edge_type, direction,
              confidence, origin, review_state, provenance,
              formula_version, edge_version,
              edge_created_at,
              endpoint_0_chunk_id, endpoint_1_chunk_id,
              endpoint_0_role, endpoint_1_role,
              change_kind, valid_from, valid_until)
           VALUES ('$EDGE3', '$EDGE3', '$CO_ACME', 'depends_on', 'directed',
                   0.8, 'administrator', 'accepted', '{}',
                   1, 1, now(),
                   '$V34_CHK1', '$V34_CHK2', 'source', 'target',
                   'update', '2026-07-04', '2026-07-01');" 2>&1 || true)
echo "$ERR" | grep -qi "check\|violates" \
  && pass "V034.20" "interval order CHECK rejects valid_until < valid_from" \
  || fail "V034.20" "reversed interval accepted: $ERR"

# V034.21: same-transaction INSERT+UPDATE+endpoints commits with exactly one
# 'insert' history row that carries the post-update values (regression for
# the mid-tx AFTER UPDATE snapshot ordering bug).
EDGE_SAMETX='6e6c0000-0000-0000-0000-0000000021a1'
sql "BEGIN;
     INSERT INTO knowledge_edges
       (id, company_id, edge_type, direction, confidence, origin)
     VALUES ('$EDGE_SAMETX', '$CO_ACME', 'implements', 'directed', 0.6,
             'administrator');
     UPDATE knowledge_edges
        SET confidence = 0.95, edge_version = edge_version + 1
        WHERE id = '$EDGE_SAMETX';
     INSERT INTO knowledge_edge_endpoints (company_id, edge_id, ordinal, chunk_id, role)
     VALUES ('$CO_ACME', '$EDGE_SAMETX', 0, '$V34_CHK1', 'source'),
            ('$CO_ACME', '$EDGE_SAMETX', 1, '$V34_CHK2', 'target');
     COMMIT;" > /dev/null 2>&1
COUNT=$(sql "SELECT count(*) FROM knowledge_edges_history WHERE live_row_id='$EDGE_SAMETX';")
KIND=$(sql "SELECT change_kind FROM knowledge_edges_history WHERE live_row_id='$EDGE_SAMETX';")
CONF=$(sql "SELECT confidence FROM knowledge_edges_history WHERE live_row_id='$EDGE_SAMETX';")
if [ "$COUNT" = "1" ] && [ "$KIND" = "insert" ] && [ "$CONF" = "0.95" ]; then
  pass "V034.21" "same-tx INSERT+UPDATE folds into single 'insert' with post-update values"
else
  fail "V034.21" "same-tx UPDATE created stray history (count=$COUNT, kind=$KIND, conf=$CONF)"
fi

# V034.22: same-transaction INSERT+DELETE leaves no history and UUID remains
# reusable (regression for the delete-path STRICT sharp edge that burned UUIDs).
EDGE_INSDEL='6e6c0000-0000-0000-0000-0000000022a1'
sql "BEGIN;
     INSERT INTO knowledge_edges
       (id, company_id, edge_type, direction, confidence, origin)
     VALUES ('$EDGE_INSDEL', '$CO_ACME', 'implements', 'directed', 0.5,
             'administrator');
     DELETE FROM knowledge_edges WHERE id = '$EDGE_INSDEL';
     COMMIT;" > /dev/null 2>&1
COUNT=$(sql "SELECT count(*) FROM knowledge_edges_history WHERE live_row_id='$EDGE_INSDEL';")
[ "$COUNT" = "0" ] \
  && pass "V034.22a" "same-tx INSERT+DELETE leaves no history rows" \
  || fail "V034.22a" "same-tx INSERT+DELETE created history (count=$COUNT)"

# Re-inserting the same UUID must be allowed (edge never existed as far as
# history is concerned).
sql "BEGIN;
     INSERT INTO knowledge_edges
       (id, company_id, edge_type, direction, confidence, origin)
     VALUES ('$EDGE_INSDEL', '$CO_ACME', 'implements', 'directed', 0.7,
             'administrator');
     INSERT INTO knowledge_edge_endpoints (company_id, edge_id, ordinal, chunk_id, role)
     VALUES ('$CO_ACME', '$EDGE_INSDEL', 0, '$V34_CHK1', 'source'),
            ('$CO_ACME', '$EDGE_INSDEL', 1, '$V34_CHK2', 'target');
     COMMIT;" > /dev/null 2>&1
COUNT=$(sql "SELECT count(*) FROM knowledge_edges WHERE id='$EDGE_INSDEL';")
[ "$COUNT" = "1" ] \
  && pass "V034.22b" "UUID reusable after same-tx INSERT+DELETE (never existed)" \
  || fail "V034.22b" "UUID incorrectly burned by same-tx INSERT+DELETE"

# V034.23 (P2 regression): DELETE FROM companies with an edge+embedding
# must succeed. outbox_events.company_id FKs to companies; without the
# guard in the BEFORE DELETE trigger the outbox INSERT fires against a
# companies row that PG has already removed, aborting the entire
# offboarding transaction with SQLSTATE 23503.
V34_CO='6e6ccca0-0000-0000-0000-000000000001'
V34_U='6e6ccca0-0000-0000-0000-0000000000a1'
sql "INSERT INTO companies (id, name, slug)
     VALUES ('$V34_CO', 'V034 Scratch', 'v034-scratch');" > /dev/null
sql "INSERT INTO users (id, company_id, external_sub, email)
     VALUES ('$V34_U', '$V34_CO', 'v34-sub', 'v34@scratch.example');" > /dev/null
V34_CO_ROOT=$(sql "SELECT id FROM org_units WHERE company_id='$V34_CO' AND type='root';")
V34_CO_DOC='6e6ccca0-0000-0000-0000-0000000000d1'
V34_CO_VER='6e6ccca0-0000-0000-0000-0000000000e1'
V34_CO_C1='6e6ccca0-0000-0000-0000-0000000000c1'
V34_CO_C2='6e6ccca0-0000-0000-0000-0000000000c2'
V34_CO_EDGE='6e6ccca0-0000-0000-0000-0000000000ed'
V34_CO_POINT='6e6ccca0-0000-0000-0000-0000000000ff'
sql "INSERT INTO documents (id, company_id, owner_org_unit_id, filename, title, mime_type)
     VALUES ('$V34_CO_DOC', '$V34_CO', '$V34_CO_ROOT', 'x.txt', 'X', 'text/plain');" > /dev/null
sql "INSERT INTO document_versions
       (id, company_id, document_id, version_no, source_hash,
        ingest_status, completed_at, activated_at, chunk_count, lifecycle_status)
     VALUES ('$V34_CO_VER', '$V34_CO', '$V34_CO_DOC', 1, 'v34co-h1',
             'done', now(), now(), 2, 'active');" > /dev/null
sql "INSERT INTO document_chunks
       (id, company_id, document_version_id, chunk_index, content, content_hash)
     VALUES ('$V34_CO_C1', '$V34_CO', '$V34_CO_VER', 0, 'a', 'ha'),
            ('$V34_CO_C2', '$V34_CO', '$V34_CO_VER', 1, 'b', 'hb');" > /dev/null
sql "BEGIN;
     INSERT INTO knowledge_edges
       (id, company_id, edge_type, direction, confidence, origin)
     VALUES ('$V34_CO_EDGE', '$V34_CO', 'implements', 'directed', 0.9,
             'administrator');
     INSERT INTO knowledge_edge_endpoints (company_id, edge_id, ordinal, chunk_id, role)
     VALUES ('$V34_CO', '$V34_CO_EDGE', 0, '$V34_CO_C1', 'source'),
            ('$V34_CO', '$V34_CO_EDGE', 1, '$V34_CO_C2', 'target');
     COMMIT;" > /dev/null
sql "INSERT INTO knowledge_edge_embeddings
       (company_id, edge_id, embedding_model_id, qdrant_point_id,
        formula_version, indexed_edge_version)
     VALUES ('$V34_CO', '$V34_CO_EDGE', '$V34_MODEL', '$V34_CO_POINT', 1, 1);" > /dev/null

# The tenant purge itself. Failure here means the offboarding trigger path
# still hits the outbox FK.
ERR=$(sql "DELETE FROM companies WHERE id='$V34_CO';" 2>&1 || true)
if [ -z "$ERR" ] || echo "$ERR" | grep -qi "DELETE"; then
  pass "V034.23" "DELETE FROM companies succeeds with edge+embedding (P2 regression)"
else
  fail "V034.23" "tenant offboarding aborted: $ERR"
fi

# V034.24 (P4 regression): same-transaction INSERT + DELETE + re-INSERT of
# one UUID must produce exactly ONE 'insert' history row. Two deferred
# parent-trigger events fire at COMMIT; the second must observe the open
# interval left by the first and return without writing a duplicate.
EDGE_REINS='6e6c0000-0000-0000-0000-0000000024a1'
sql "BEGIN;
     INSERT INTO knowledge_edges
       (id, company_id, edge_type, direction, confidence, origin)
     VALUES ('$EDGE_REINS', '$CO_ACME', 'implements', 'directed', 0.5,
             'administrator');
     INSERT INTO knowledge_edge_endpoints (company_id, edge_id, ordinal, chunk_id, role)
     VALUES ('$CO_ACME', '$EDGE_REINS', 0, '$V34_CHK1', 'source'),
            ('$CO_ACME', '$EDGE_REINS', 1, '$V34_CHK2', 'target');
     DELETE FROM knowledge_edges WHERE id='$EDGE_REINS';
     INSERT INTO knowledge_edges
       (id, company_id, edge_type, direction, confidence, origin)
     VALUES ('$EDGE_REINS', '$CO_ACME', 'depends_on', 'directed', 0.9,
             'administrator');
     INSERT INTO knowledge_edge_endpoints (company_id, edge_id, ordinal, chunk_id, role)
     VALUES ('$CO_ACME', '$EDGE_REINS', 0, '$V34_CHK1', 'source'),
            ('$CO_ACME', '$EDGE_REINS', 1, '$V34_CHK3', 'target');
     COMMIT;" > /dev/null 2>&1
COUNT=$(sql "SELECT count(*) FROM knowledge_edges_history
             WHERE live_row_id='$EDGE_REINS' AND change_kind='insert';")
[ "$COUNT" = "1" ] \
  && pass "V034.24" "re-INSERT after same-tx DELETE emits exactly one 'insert' history row" \
  || fail "V034.24" "duplicate 'insert' history rows (count=$COUNT)"

# V034.25 (P3 sanity): both trigger functions that call snapshot_internal
# are SECURITY DEFINER. Otherwise the REVOKE on snapshot_internal either
# has no teeth (owner bypass) or hard-breaks the trigger path once
# migration/app roles are split.
COUNT=$(sql "SELECT count(*) FROM pg_proc p
             JOIN pg_namespace n ON n.oid = p.pronamespace
             WHERE n.nspname = 'public'
               AND p.proname IN ('knowledge_edges_check_count_and_snapshot_fn',
                                 'knowledge_edges_history_update_fn')
               AND p.prosecdef = true;")
[ "$COUNT" = "2" ] \
  && pass "V034.25" "trigger functions calling snapshot_internal are SECURITY DEFINER" \
  || fail "V034.25" "SECURITY DEFINER count wrong (got $COUNT, expected 2)"
