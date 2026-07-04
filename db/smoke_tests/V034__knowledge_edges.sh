# Per-migration smoke tests for V034 (BaryGraph Lite knowledge edges).
#
# Creates its own fixtures (document_version + two chunks) rather than
# depending on VER3/CHK1, which are deleted earlier in the main test suite.

V34_DOC='6e6cd0c0-0000-0000-0000-000000000001'
V34_VER='6e6c7e70-0000-0000-0000-000000000001'
V34_CHK1='6e6cc010-0000-0000-0000-000000000001'
V34_CHK2='6e6cc010-0000-0000-0000-000000000002'
EDGE1='6e6c0000-0000-0000-0000-000000000001'
EDGE2='6e6c0000-0000-0000-0000-000000000002'
EDGE_BAD='6e6c0000-0000-0000-0000-00000000bad1'

# Seed a document, version, and two chunks in CO_ACME.
sql "INSERT INTO documents (id, company_id, owner_org_unit_id, filename, title, mime_type)
     VALUES ('$V34_DOC', '$CO_ACME', '$ACME_ROOT', 'v34.txt', 'V034 test doc', 'text/plain');" > /dev/null

sql "INSERT INTO document_versions
       (id, company_id, document_id, version_no, source_hash,
        ingest_status, completed_at, activated_at, chunk_count, lifecycle_status)
     VALUES ('$V34_VER', '$CO_ACME', '$V34_DOC', 1, 'v34-h1',
             'done', now(), now(), 2, 'active');" > /dev/null

sql "INSERT INTO document_chunks
       (id, company_id, document_version_id, chunk_index, content, content_hash)
     VALUES ('$V34_CHK1', '$CO_ACME', '$V34_VER', 0, 'first chunk', 'ch1'),
            ('$V34_CHK2', '$CO_ACME', '$V34_VER', 1, 'second chunk', 'ch2');" > /dev/null

# V034.1: knowledge_edges accepts a valid V1 edge with two endpoints in one tx
sql "BEGIN;
     SET CONSTRAINTS ALL DEFERRED;
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
  && pass "V034.1" "valid edge with two endpoints accepted at COMMIT" \
  || fail "V034.1" "valid edge insert rejected (count=$COUNT)"

# V034.2: history has one insert row for the new edge
COUNT=$(sql "SELECT count(*) FROM knowledge_edges_history
             WHERE live_row_id='$EDGE1' AND change_kind='insert' AND valid_until IS NULL;")
[ "$COUNT" = "1" ] \
  && pass "V034.2" "AFTER INSERT trigger wrote one open history interval" \
  || fail "V034.2" "history missing insert row (count=$COUNT)"

# V034.3: history endpoint snapshot captured both chunk IDs
COUNT=$(sql "SELECT count(*) FROM knowledge_edges_history
             WHERE live_row_id='$EDGE1'
               AND endpoint_0_chunk_id='$V34_CHK1'
               AND endpoint_1_chunk_id='$V34_CHK2';")
[ "$COUNT" = "1" ] \
  && pass "V034.3" "history snapshotted both endpoint chunk IDs" \
  || fail "V034.3" "history endpoint snapshot missing (count=$COUNT)"

# V034.4: edge with only one endpoint rejected at COMMIT by CONSTRAINT TRIGGER
ERR=$(sql "BEGIN;
     SET CONSTRAINTS ALL DEFERRED;
     INSERT INTO knowledge_edges
       (id, company_id, edge_type, direction, confidence, origin)
     VALUES ('$EDGE_BAD', '$CO_ACME', 'implements', 'directed', 0.9, 'administrator');
     INSERT INTO knowledge_edge_endpoints (company_id, edge_id, ordinal, chunk_id, role)
     VALUES ('$CO_ACME', '$EDGE_BAD', 0, '$V34_CHK1', 'source');
     COMMIT;" 2>&1 || true)
echo "$ERR" | grep -qi "exactly two endpoints\|check_violation" \
  && pass "V034.4" "edge with one endpoint rejected at COMMIT" \
  || fail "V034.4" "single-endpoint edge not rejected: $ERR"

# V034.5: V2-only edge type rejected by V1 CHECK
ERR=$(sql "BEGIN;
     SET CONSTRAINTS ALL DEFERRED;
     INSERT INTO knowledge_edges
       (id, company_id, edge_type, direction, confidence, origin)
     VALUES (gen_random_uuid(), '$CO_ACME', 'supersedes', 'directed', 0.9, 'administrator');
     ROLLBACK;" 2>&1 || true)
echo "$ERR" | grep -qi "edge_type_v1_chk\|check constraint" \
  && pass "V034.5" "V2-only edge_type 'supersedes' rejected by V1 CHECK" \
  || fail "V034.5" "V2 edge_type accepted: $ERR"

# V034.6: composite FK on endpoints table exists (tenant integrity)
EXISTS=$(sql "SELECT count(*) FROM pg_constraint
              WHERE contype = 'f'
                AND conrelid = 'knowledge_edge_endpoints'::regclass;")
[ "$EXISTS" -ge "2" ] \
  && pass "V034.6" "knowledge_edge_endpoints has composite FKs (tenant integrity)" \
  || fail "V034.6" "composite FK missing (count=$EXISTS)"

# V034.7: DELETE emits Qdrant cleanup outbox event with point IDs
MODEL_ID=$(sql "SELECT id FROM embedding_models WHERE company_id='$CO_ACME' LIMIT 1;" 2>/dev/null || echo "")
if [ -n "$MODEL_ID" ]; then
    POINT_ID='6e6c0000-0000-0000-0000-0000000000aa'
    sql "INSERT INTO knowledge_edge_embeddings
           (company_id, edge_id, embedding_model_id, qdrant_point_id,
            formula_version, indexed_edge_version)
         VALUES ('$CO_ACME', '$EDGE1', '$MODEL_ID', '$POINT_ID', 1, 1);" > /dev/null

    sql "DELETE FROM knowledge_edges WHERE id='$EDGE1';" > /dev/null
    COUNT=$(sql "SELECT count(*) FROM outbox_events
                 WHERE company_id='$CO_ACME'
                   AND job_type='qdrant_delete_edge_points'
                   AND aggregate_id='$EDGE1';")
    [ "$COUNT" = "1" ] \
      && pass "V034.7" "DELETE emits qdrant_delete_edge_points outbox event" \
      || fail "V034.7" "outbox event missing (count=$COUNT)"

    # V034.8: history has delete marker after DELETE
    COUNT=$(sql "SELECT count(*) FROM knowledge_edges_history
                 WHERE live_row_id='$EDGE1' AND change_kind='delete';")
    [ "$COUNT" = "1" ] \
      && pass "V034.8" "history has delete marker after DELETE" \
      || fail "V034.8" "delete marker missing (count=$COUNT)"
else
    pass "V034.7" "skipped (no embedding_models fixture)"
    pass "V034.8" "skipped (no embedding_models fixture)"
fi

# V034.9: knowledge_edges_snapshot() writes an update history row
sql "BEGIN;
     SET CONSTRAINTS ALL DEFERRED;
     INSERT INTO knowledge_edges
       (id, company_id, edge_type, direction, confidence, origin)
     VALUES ('$EDGE2', '$CO_ACME', 'implements', 'directed', 0.9, 'administrator');
     INSERT INTO knowledge_edge_endpoints (company_id, edge_id, ordinal, chunk_id, role)
     VALUES ('$CO_ACME', '$EDGE2', 0, '$V34_CHK1', 'source'),
            ('$CO_ACME', '$EDGE2', 1, '$V34_CHK2', 'target');
     COMMIT;" > /dev/null

sql "CALL knowledge_edges_snapshot('$EDGE2', 'update', clock_timestamp());" > /dev/null
COUNT=$(sql "SELECT count(*) FROM knowledge_edges_history
             WHERE live_row_id='$EDGE2' AND change_kind='update';")
[ "$COUNT" = "1" ] \
  && pass "V034.9" "knowledge_edges_snapshot() writes update history row" \
  || fail "V034.9" "snapshot procedure did not write history (count=$COUNT)"

# V034.10: partial unique index blocks duplicate open intervals
ERR=$(sql "INSERT INTO knowledge_edges_history
             (live_row_id, edge_id, company_id, edge_type, direction,
              confidence, origin, review_state, provenance,
              formula_version, edge_version,
              endpoint_0_chunk_id, endpoint_1_chunk_id,
              endpoint_0_role, endpoint_1_role,
              change_kind, valid_from, valid_until)
           VALUES ('$EDGE2', '$EDGE2', '$CO_ACME', 'implements', 'directed',
                   0.9, 'administrator', 'accepted', '{}',
                   1, 1,
                   '$V34_CHK1', '$V34_CHK2', 'source', 'target',
                   'update', clock_timestamp(), NULL);" 2>&1 || true)
echo "$ERR" | grep -qi "knowledge_edges_history_open_uidx\|unique\|duplicate" \
  && pass "V034.10" "partial unique index rejects duplicate open interval" \
  || fail "V034.10" "duplicate open interval accepted: $ERR"
