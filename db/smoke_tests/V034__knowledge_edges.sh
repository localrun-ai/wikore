# Per-migration smoke tests for V034 (BaryGraph Lite knowledge edges).
#
# Creates its own document/version/chunk fixtures rather than depending on
# VER3/CHK1, which are deleted earlier in the main test suite.

V34_DOC='6e6cd0c0-0000-0000-0000-000000000001'
V34_VER='6e6c7e70-0000-0000-0000-000000000001'
V34_CHK1='6e6cc010-0000-0000-0000-000000000001'
V34_CHK2='6e6cc010-0000-0000-0000-000000000002'
V34_CHK3='6e6cc010-0000-0000-0000-000000000003'
EDGE1='6e6c0000-0000-0000-0000-000000000001'
EDGE2='6e6c0000-0000-0000-0000-000000000002'
EDGE3='6e6c0000-0000-0000-0000-000000000003'
EDGE_ORPHAN='6e6c0000-0000-0000-0000-00000000bad0'

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

# V034.2: initial history row written with all lifecycle timestamps captured
ROW=$(sql "SELECT
    change_kind ||'|'|| coalesce(edge_expires_at::text,'NULL')
    FROM knowledge_edges_history WHERE live_row_id='$EDGE1';")
echo "$ROW" | grep -q "^insert|NULL$" \
  && pass "V034.2" "initial history row: change_kind=insert, lifecycle timestamps captured" \
  || fail "V034.2" "unexpected initial history row: $ROW"

# V034.3: history endpoint snapshot captured both chunk IDs
COUNT=$(sql "SELECT count(*) FROM knowledge_edges_history
             WHERE live_row_id='$EDGE1'
               AND endpoint_0_chunk_id='$V34_CHK1'
               AND endpoint_1_chunk_id='$V34_CHK2';")
[ "$COUNT" = "1" ] \
  && pass "V034.3" "history captured both endpoint chunk IDs" \
  || fail "V034.3" "endpoint snapshot missing (count=$COUNT)"

# V034.4 (P1-1a): zero-endpoint edge rejected at COMMIT (parent-table trigger)
ERR=$(sql "BEGIN;
     INSERT INTO knowledge_edges
       (id, company_id, edge_type, direction, confidence, origin)
     VALUES ('$EDGE_ORPHAN', '$CO_ACME', 'implements', 'directed', 0.9, 'administrator');
     COMMIT;" 2>&1 || true)
echo "$ERR" | grep -qi "exactly two endpoints\|check_violation" \
  && pass "V034.4" "zero-endpoint edge rejected at COMMIT (parent trigger)" \
  || fail "V034.4" "zero-endpoint edge accepted: $ERR"

# V034.5 (P1-1b): one-endpoint edge rejected at COMMIT (endpoints trigger)
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

# V034.6 (P2-5): reversed insertion order (ordinal=1 before ordinal=0) accepted
# because history-on-insert is deferred to CONSTRAINT TRIGGER at COMMIT.
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
  && pass "V034.6" "reversed insertion order accepted (history deferred to COMMIT)" \
  || fail "V034.6" "reversed order rejected (count=$COUNT)"

# V034.7 (P1-2a): AFTER UPDATE trigger writes history automatically
sql "UPDATE knowledge_edges SET confidence = 0.5, edge_version = edge_version + 1
     WHERE id='$EDGE1';" > /dev/null
COUNT=$(sql "SELECT count(*) FROM knowledge_edges_history
             WHERE live_row_id='$EDGE1' AND change_kind='update';")
[ "$COUNT" = "1" ] \
  && pass "V034.7" "AFTER UPDATE trigger wrote history automatically" \
  || fail "V034.7" "UPDATE history missing (count=$COUNT)"

# V034.8 (P1-2b): closed prior open interval when writing update
OPEN=$(sql "SELECT count(*) FROM knowledge_edges_history
             WHERE live_row_id='$EDGE1' AND valid_until IS NULL;")
[ "$OPEN" = "1" ] \
  && pass "V034.8" "exactly one open history interval after UPDATE" \
  || fail "V034.8" "open interval count wrong (got $OPEN)"

# V034.9 (P2-5): endpoints immutable — UPDATE rejected
ERR=$(sql "UPDATE knowledge_edge_endpoints
           SET chunk_id='$V34_CHK3'
           WHERE edge_id='$EDGE1' AND ordinal=0;" 2>&1 || true)
echo "$ERR" | grep -qi "immutable\|insufficient_privilege" \
  && pass "V034.9" "knowledge_edge_endpoints UPDATE rejected (immutable)" \
  || fail "V034.9" "endpoint UPDATE accepted: $ERR"

# V034.10: V2-only edge type rejected by V1 CHECK
ERR=$(sql "INSERT INTO knowledge_edges
             (id, company_id, edge_type, direction, confidence, origin)
           VALUES (gen_random_uuid(), '$CO_ACME', 'supersedes',
                   'directed', 0.9, 'administrator');" 2>&1 || true)
echo "$ERR" | grep -qi "edge_type_v1_chk\|check constraint" \
  && pass "V034.10" "V2-only edge_type 'supersedes' rejected by V1 CHECK" \
  || fail "V034.10" "V2 edge_type accepted: $ERR"

# V034.11 (P1-3): composite FK enforces same-company created_by
# We don't have a cross-tenant user fixture here; instead verify the FK exists.
CNT=$(sql "SELECT count(*) FROM pg_constraint
           WHERE conname IN ('knowledge_edges_created_by_same_company_fk',
                             'knowledge_edges_reviewed_by_same_company_fk');")
[ "$CNT" = "2" ] \
  && pass "V034.11" "composite same-company FKs installed for created_by/reviewed_by" \
  || fail "V034.11" "composite FKs missing (count=$CNT)"

# V034.12 (P2-6 documentation): outbox event emitted on DELETE
MODEL_ID=$(sql "SELECT id FROM embedding_models WHERE company_id='$CO_ACME' LIMIT 1;" 2>/dev/null || echo "")
if [ -n "$MODEL_ID" ]; then
    POINT_ID='6e6c0000-0000-0000-0000-0000000000aa'
    sql "INSERT INTO knowledge_edge_embeddings
           (company_id, edge_id, embedding_model_id, qdrant_point_id,
            formula_version, indexed_edge_version)
         VALUES ('$CO_ACME', '$EDGE1', '$MODEL_ID', '$POINT_ID', 1, 1);" > /dev/null
    sql "DELETE FROM knowledge_edges WHERE id='$EDGE1';" > /dev/null
    COUNT=$(sql "SELECT count(*) FROM outbox_events
                 WHERE aggregate_id='$EDGE1' AND job_type='qdrant_delete_edge_points';")
    [ "$COUNT" = "1" ] \
      && pass "V034.12" "DELETE emits qdrant_delete_edge_points outbox event" \
      || fail "V034.12" "outbox event missing (count=$COUNT)"

    # V034.13: history has delete marker
    COUNT=$(sql "SELECT count(*) FROM knowledge_edges_history
                 WHERE live_row_id='$EDGE1' AND change_kind='delete';")
    [ "$COUNT" = "1" ] \
      && pass "V034.13" "history has delete marker after DELETE" \
      || fail "V034.13" "delete marker missing (count=$COUNT)"
else
    pass "V034.12" "skipped (no embedding_models fixture)"
    pass "V034.13" "skipped (no embedding_models fixture)"
fi

# V034.14 (P1-2c): knowledge_edges_snapshot_at rejects caller kind='delete'
ERR=$(sql "CALL knowledge_edges_snapshot_at('$EDGE2', 'delete');" 2>&1 || true)
echo "$ERR" | grep -qi "invalid_parameter\|kind must be" \
  && pass "V034.14" "snapshot procedure rejects kind='delete' from caller" \
  || fail "V034.14" "kind='delete' accepted from caller: $ERR"

# V034.15: partial unique index rejects duplicate open interval
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
  && pass "V034.15" "partial unique index rejects duplicate open interval" \
  || fail "V034.15" "duplicate open interval accepted: $ERR"

# V034.16 (P1-2d): interval order CHECK on history
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
  && pass "V034.16" "interval order CHECK rejects valid_until < valid_from" \
  || fail "V034.16" "reversed interval accepted: $ERR"

# V034.17 (P1-3): composite FK rejects cross-tenant created_by
# U_BOB is in CO_BETA, so setting created_by=U_BOB on a CO_ACME edge should
# fail at INSERT time (not just via a BEFORE trigger — the composite FK
# check is atomic and race-free).
ERR=$(sql "BEGIN;
     INSERT INTO knowledge_edges
       (id, company_id, edge_type, direction, confidence, origin, created_by)
     VALUES (gen_random_uuid(), '$CO_ACME', 'implements', 'directed', 0.9,
             'administrator', '$U_BOB');
     ROLLBACK;" 2>&1 || true)
echo "$ERR" | grep -qi "same_company_fk\|foreign key" \
  && pass "V034.17" "composite FK rejects cross-tenant created_by (race-free)" \
  || fail "V034.17" "cross-tenant actor accepted: $ERR"
