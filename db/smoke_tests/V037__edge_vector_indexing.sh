# Per-migration smoke tests for V037 (BaryGraph Lite edge-vector indexing).

V37_CO='7e370000-0000-0000-0000-0000000000c1'
V37_DOC='7e370d00-0000-0000-0000-000000000001'
V37_VER='7e377e00-0000-0000-0000-000000000001'
V37_C1='7e37c800-0000-0000-0000-000000000001'
V37_C2='7e37c800-0000-0000-0000-000000000002'
V37_MODEL='7e37ee00-0000-0000-0000-000000000001'
V37_MODEL2='7e37ee00-0000-0000-0000-000000000002'
V37_ADMIN='7e37a700-0000-0000-0000-000000000001'

# Fresh scratch tenant + chunks (V037 tests share the DB with earlier
# migrations, so pick UUIDs that will not collide with V034/V036 seeds).
sql "INSERT INTO companies (id,name,slug) VALUES ('$V37_CO','V037 Scratch','v037-scratch');" > /dev/null
V37_ROOT=$(sql "SELECT id FROM org_units WHERE company_id='$V37_CO' AND type='root';")
sql "INSERT INTO users (id,company_id,external_sub,email,is_admin)
     VALUES ('$V37_ADMIN','$V37_CO','sub-v37','v37@scratch.example',true);" > /dev/null

sql "INSERT INTO embedding_models (id,name,qdrant_collection,dimension,enabled)
     VALUES ('$V37_MODEL','v037-model-a','wikore_v037_a',4,true);" > /dev/null
sql "INSERT INTO embedding_models (id,name,qdrant_collection,dimension,enabled)
     VALUES ('$V37_MODEL2','v037-model-b','wikore_v037_b',4,false);" > /dev/null

sql "INSERT INTO documents (id,company_id,owner_org_unit_id,filename,title,mime_type)
     VALUES ('$V37_DOC','$V37_CO','$V37_ROOT','r.txt','R','text/plain');" > /dev/null
sql "INSERT INTO document_versions
       (id,company_id,document_id,version_no,source_hash,
        ingest_status,completed_at,activated_at,chunk_count,lifecycle_status)
     VALUES ('$V37_VER','$V37_CO','$V37_DOC',1,'h','done',now(),now(),2,'active');" > /dev/null
sql "INSERT INTO document_chunks (id,company_id,document_version_id,chunk_index,content,content_hash)
     VALUES ('$V37_C1','$V37_CO','$V37_VER',0,'a','ha'),
            ('$V37_C2','$V37_CO','$V37_VER',1,'b','hb');" > /dev/null

# V037.1: edge_type_vectors accepts a valid registry row
sql "INSERT INTO edge_type_vectors
       (formula_version,edge_type,embedding_model_id,vector,description)
     VALUES (1,'implements','$V37_MODEL','{0.1,0.2,0.3,0.4}',
             'one governed procedure operationalises and fulfils the other requirement');" > /dev/null
COUNT=$(sql "SELECT count(*) FROM edge_type_vectors WHERE embedding_model_id='$V37_MODEL';")
[ "$COUNT" = "1" ] \
  && pass "V037.1" "edge_type_vectors accepts a valid registry row" \
  || fail "V037.1" "valid registry row rejected (count=$COUNT)"

# V037.2: PK rejects duplicate (formula_version, edge_type, embedding_model_id)
ERR=$(sql "INSERT INTO edge_type_vectors
             (formula_version,edge_type,embedding_model_id,vector,description)
           VALUES (1,'implements','$V37_MODEL','{0.5,0.5,0.5,0.5}','dup');" 2>&1 || true)
echo "$ERR" | grep -qi "duplicate\|unique\|pkey" \
  && pass "V037.2" "edge_type_vectors PK rejects duplicate registry row" \
  || fail "V037.2" "duplicate registry row accepted: $ERR"

# V037.3: same edge_type, different formula_version is fine
sql "INSERT INTO edge_type_vectors
       (formula_version,edge_type,embedding_model_id,vector,description)
     VALUES (2,'implements','$V37_MODEL','{0.1,0.1,0.1,0.1}',
             'formula v2 wording of the same edge type');" > /dev/null
COUNT=$(sql "SELECT count(*) FROM edge_type_vectors WHERE edge_type='implements';")
[ "$COUNT" = "2" ] \
  && pass "V037.3" "same edge_type across formula_versions accepted" \
  || fail "V037.3" "cross-version rejected (count=$COUNT)"

# V037.4: empty vector rejected by CHECK
ERR=$(sql "INSERT INTO edge_type_vectors
             (formula_version,edge_type,embedding_model_id,vector,description)
           VALUES (1,'depends_on','$V37_MODEL','{}','empty vector should fail');" 2>&1 || true)
echo "$ERR" | grep -qi "vector_dim_positive\|check constraint" \
  && pass "V037.4" "empty vector rejected by CHECK" \
  || fail "V037.4" "empty vector accepted: $ERR"

# V037.5: description length CHECK rejects too-long text
LONG=$(printf 'x%.0s' $(seq 1 600))
ERR=$(sql "INSERT INTO edge_type_vectors
             (formula_version,edge_type,embedding_model_id,vector,description)
           VALUES (1,'contradicts','$V37_MODEL','{0.1,0.1,0.1,0.1}','$LONG');" 2>&1 || true)
echo "$ERR" | grep -qi "check constraint" \
  && pass "V037.5" "description length CHECK rejects >512 chars" \
  || fail "V037.5" "long description accepted: $ERR"

# V037.6: FK to embedding_models rejects unknown model id
ERR=$(sql "INSERT INTO edge_type_vectors
             (formula_version,edge_type,embedding_model_id,vector,description)
           VALUES (1,'derived_from','7e370000-0000-0000-0000-000000ffffff',
                   '{0.1,0.1,0.1,0.1}','x');" 2>&1 || true)
echo "$ERR" | grep -qi "foreign key\|violates" \
  && pass "V037.6" "FK rejects unknown embedding_model_id" \
  || fail "V037.6" "unknown model accepted: $ERR"

# ---------------------------------------------------------------------------
# Trigger: knowledge_edges INSERT enqueues qdrant_upsert_edge_vector for
# every ENABLED embedding_model (and only enabled ones).
# ---------------------------------------------------------------------------

V37_EDGE1='7e376c00-0000-0000-0000-000000000001'
sql "WITH e AS ( \
  INSERT INTO knowledge_edges \
    (id,company_id,edge_type,direction,confidence,origin) \
  VALUES ('$V37_EDGE1','$V37_CO','implements','directed',0.9,'administrator') \
  RETURNING id, company_id \
) \
INSERT INTO knowledge_edge_endpoints (company_id, edge_id, ordinal, chunk_id, role) \
SELECT company_id, id, 0, '$V37_C1'::uuid, 'source' FROM e \
UNION ALL \
SELECT company_id, id, 1, '$V37_C2'::uuid, 'target' FROM e;" > /dev/null

# Scope every following count() to V37_MODEL — other smoke tests (or a
# prior V037.1 run) may have registered other enabled models globally.
COUNT=$(sql "SELECT count(*) FROM outbox_events
             WHERE aggregate_id='$V37_EDGE1'
               AND job_type='qdrant_upsert_edge_vector'
               AND payload->>'embedding_model_id'='$V37_MODEL';")
[ "$COUNT" = "1" ] \
  && pass "V037.7" "INSERT enqueues exactly one event per enabled model" \
  || fail "V037.7" "wrong event count on INSERT for model (got $COUNT, expected 1)"

# V037.8: disabled embedding model does NOT produce an event
COUNT=$(sql "SELECT count(*) FROM outbox_events
             WHERE aggregate_id='$V37_EDGE1'
               AND job_type='qdrant_upsert_edge_vector'
               AND payload->>'embedding_model_id'='$V37_MODEL2';")
[ "$COUNT" = "0" ] \
  && pass "V037.8" "disabled embedding_model excluded from enqueue" \
  || fail "V037.8" "disabled model produced $COUNT events"

# V037.9: enabled model IS the source of the enqueued event
COUNT=$(sql "SELECT count(*) FROM outbox_events
             WHERE aggregate_id='$V37_EDGE1'
               AND job_type='qdrant_upsert_edge_vector'
               AND payload->>'embedding_model_id'='$V37_MODEL';")
[ "$COUNT" = "1" ] \
  && pass "V037.9" "enabled model produces the enqueued event" \
  || fail "V037.9" "enabled model did not produce event (got $COUNT)"

# V037.10: payload carries formula_version, edge_version, edge_type
FV=$(sql "SELECT payload->>'formula_version' FROM outbox_events
          WHERE aggregate_id='$V37_EDGE1'
            AND job_type='qdrant_upsert_edge_vector'
            AND payload->>'embedding_model_id'='$V37_MODEL';")
EV=$(sql "SELECT payload->>'edge_version' FROM outbox_events
          WHERE aggregate_id='$V37_EDGE1'
            AND job_type='qdrant_upsert_edge_vector'
            AND payload->>'embedding_model_id'='$V37_MODEL';")
ET=$(sql "SELECT payload->>'edge_type' FROM outbox_events
          WHERE aggregate_id='$V37_EDGE1'
            AND job_type='qdrant_upsert_edge_vector'
            AND payload->>'embedding_model_id'='$V37_MODEL';")
if [ "$FV" = "1" ] && [ "$EV" = "1" ] && [ "$ET" = "implements" ]; then
  pass "V037.10" "payload carries formula_version + edge_version + edge_type"
else
  fail "V037.10" "payload missing fields (fv=$FV, ev=$EV, et=$ET)"
fi

# V037.11: UPDATE that bumps edge_version enqueues a second event
sql "UPDATE knowledge_edges
     SET confidence=0.8, edge_version=edge_version+1
     WHERE id='$V37_EDGE1';" > /dev/null
COUNT=$(sql "SELECT count(*) FROM outbox_events
             WHERE aggregate_id='$V37_EDGE1'
               AND job_type='qdrant_upsert_edge_vector'
               AND payload->>'embedding_model_id'='$V37_MODEL';")
[ "$COUNT" = "2" ] \
  && pass "V037.11" "UPDATE bumping edge_version enqueues a second event" \
  || fail "V037.11" "UPDATE event count wrong (got $COUNT, expected 2)"

# V037.12: UPDATE that does NOT bump edge_version is a no-op
sql "UPDATE knowledge_edges SET expires_at=NULL WHERE id='$V37_EDGE1';" > /dev/null
COUNT=$(sql "SELECT count(*) FROM outbox_events
             WHERE aggregate_id='$V37_EDGE1'
               AND job_type='qdrant_upsert_edge_vector'
               AND payload->>'embedding_model_id'='$V37_MODEL';")
[ "$COUNT" = "2" ] \
  && pass "V037.12" "UPDATE without edge_version bump does not enqueue" \
  || fail "V037.12" "same-version UPDATE created $COUNT events (expected 2)"

# V037.13: idempotency_key collapses same-version retriggers
# (the ON CONFLICT (company_id, job_type, idempotency_key) DO NOTHING)
sql "UPDATE knowledge_edges
     SET review_state='rejected', edge_version=edge_version+1
     WHERE id='$V37_EDGE1';" > /dev/null
# Now edge_version = 3. Fire another same-version UPDATE — the outbox
# count MUST still be 3 (one per distinct edge_version), not 4.
sql "UPDATE knowledge_edges SET review_state='rejected' WHERE id='$V37_EDGE1';" > /dev/null
COUNT=$(sql "SELECT count(*) FROM outbox_events
             WHERE aggregate_id='$V37_EDGE1'
               AND job_type='qdrant_upsert_edge_vector'
               AND payload->>'embedding_model_id'='$V37_MODEL';")
[ "$COUNT" = "3" ] \
  && pass "V037.13" "idempotency_key collapses same-version retriggers" \
  || fail "V037.13" "duplicate events emitted (got $COUNT, expected 3)"

# V037.14: DELETE FROM knowledge_edges does NOT emit an upsert event
# (V034's BEFORE DELETE trigger emits qdrant_delete_edge_points instead).
sql "DELETE FROM knowledge_edges WHERE id='$V37_EDGE1';" > /dev/null
COUNT=$(sql "SELECT count(*) FROM outbox_events
             WHERE aggregate_id='$V37_EDGE1'
               AND job_type='qdrant_upsert_edge_vector'
               AND payload->>'embedding_model_id'='$V37_MODEL';")
[ "$COUNT" = "3" ] \
  && pass "V037.14" "DELETE does not emit upsert event (still 3 from history)" \
  || fail "V037.14" "DELETE altered upsert-event count (got $COUNT)"

# V037.15: enqueue function is not callable by public
NO_PUBLIC=$(sql "SELECT NOT EXISTS (
    SELECT 1 FROM information_schema.role_routine_grants
    WHERE routine_name='knowledge_edges_enqueue_upsert_fn'
      AND grantee='PUBLIC'
);")
[ "$NO_PUBLIC" = "t" ] \
  && pass "V037.15" "enqueue function has no PUBLIC EXECUTE grant" \
  || fail "V037.15" "PUBLIC can execute enqueue function"
