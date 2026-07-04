-- V034: BaryGraph Lite knowledge edges (chunk-to-chunk semantic relationships)
--
-- First of three BaryGraph Lite migrations (see docs/barygraph_features.md).
-- V035 adds capability grants; V036 adds privileged access sessions.
--
-- Correctness invariants enforced at the database level:
--
--   * Exactly two endpoints per edge at COMMIT — parent-table CONSTRAINT
--     TRIGGER catches zero endpoints; endpoints-table trigger catches one
--     endpoint.  Both DEFERRABLE INITIALLY DEFERRED.
--
--   * Endpoints are effectively immutable.  UPDATE and DELETE (while the
--     parent still exists) are rejected.  Endpoint changes require deleting
--     the edge and recreating it — matching the "edges are superseded not
--     modified" design principle.  The DELETE guard eliminates a bypass
--     where a caller could DELETE + INSERT endpoints with a preserved count
--     and no audit snapshot.
--
--   * Edge UUIDs cannot be reused.  If knowledge_edges_history rows for a
--     live_row_id already exist, INSERT of a new knowledge_edges row with
--     that UUID is rejected — old history would otherwise mask the new
--     initial-insert snapshot.
--
--   * Same-company actors enforced by composite foreign keys, not a BEFORE
--     trigger (which had a concurrency race under a users.company_id UPDATE).
--
--   * History is trigger-owned.  Initial insert is written by the parent
--     CONSTRAINT TRIGGER (only path).  Updates by AFTER UPDATE trigger.
--     Deletes by BEFORE DELETE trigger.  The internal snapshot function is
--     REVOKEd from PUBLIC so callers cannot forge history transitions.
--
--   * knowledge_edges_snapshot_internal() captures clock_timestamp() AFTER
--     acquiring the parent lock, so a waiting snapshot cannot backdate its
--     interval below the transaction it waited for.

-- ---------------------------------------------------------------------------
-- knowledge_edges
-- ---------------------------------------------------------------------------

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
    created_by          UUID,
    reviewed_by         UUID,
    created_at          TIMESTAMPTZ NOT NULL DEFAULT now(),
    reviewed_at         TIMESTAMPTZ,
    expires_at          TIMESTAMPTZ,
    superseded_at       TIMESTAMPTZ,
    CHECK (expires_at IS NULL OR expires_at > created_at),
    UNIQUE (company_id, id),
    CONSTRAINT knowledge_edges_created_by_same_company_fk
        FOREIGN KEY (company_id, created_by)
        REFERENCES users(company_id, id) MATCH SIMPLE
        ON DELETE NO ACTION,
    CONSTRAINT knowledge_edges_reviewed_by_same_company_fk
        FOREIGN KEY (company_id, reviewed_by)
        REFERENCES users(company_id, id) MATCH SIMPLE
        ON DELETE NO ACTION
);

-- V1 chunk-to-chunk edge types only.
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

CREATE INDEX knowledge_edges_company_idx     ON knowledge_edges (company_id);
CREATE INDEX knowledge_edges_edge_type_idx   ON knowledge_edges (company_id, edge_type);
CREATE INDEX knowledge_edges_review_state_idx
    ON knowledge_edges (company_id, review_state)
    WHERE review_state = 'accepted';
CREATE INDEX knowledge_edges_created_by_idx  ON knowledge_edges (created_by)
    WHERE created_by IS NOT NULL;
CREATE INDEX knowledge_edges_reviewed_by_idx ON knowledge_edges (reviewed_by)
    WHERE reviewed_by IS NOT NULL;

-- ---------------------------------------------------------------------------
-- knowledge_edge_endpoints (chunk-to-chunk only; endpoints immutable)
-- ---------------------------------------------------------------------------

CREATE TABLE knowledge_edge_endpoints (
    company_id  UUID     NOT NULL,
    edge_id     UUID     NOT NULL,
    ordinal     SMALLINT NOT NULL CHECK (ordinal IN (0, 1)),
    chunk_id    UUID     NOT NULL,
    role        TEXT     NOT NULL
                    CHECK (role IN ('source','target','subject','object','a','b')),
    PRIMARY KEY (edge_id, ordinal),
    UNIQUE (edge_id, chunk_id),
    FOREIGN KEY (company_id, edge_id)
        REFERENCES knowledge_edges(company_id, id) ON DELETE CASCADE,
    FOREIGN KEY (company_id, chunk_id)
        REFERENCES document_chunks(company_id, id) ON DELETE RESTRICT
);

CREATE INDEX knowledge_edge_endpoints_chunk_idx
    ON knowledge_edge_endpoints (company_id, chunk_id);

-- Endpoints are immutable AND cannot be independently deleted while their
-- parent edge still exists (only cascade may remove them). This closes the
-- DELETE + INSERT bypass where a caller could swap endpoints while
-- preserving the 2-count and skipping the initial-history duplicate check.
CREATE OR REPLACE FUNCTION knowledge_edge_endpoints_reject_write_fn()
RETURNS TRIGGER LANGUAGE plpgsql AS $$
BEGIN
    IF TG_OP = 'UPDATE' THEN
        RAISE EXCEPTION
            'knowledge_edge_endpoints is immutable; UPDATE rejected. '
            'Delete and recreate the edge instead.'
            USING ERRCODE = 'insufficient_privilege',
                  CONSTRAINT = 'knowledge_edge_endpoints_immutable';
    ELSE  -- DELETE
        -- Allow DELETE only when the parent edge no longer exists (cascade path).
        IF EXISTS (SELECT 1 FROM knowledge_edges WHERE id = OLD.edge_id) THEN
            RAISE EXCEPTION
                'knowledge_edge_endpoints cannot be deleted while parent edge exists; '
                'delete the parent edge instead (cascade will remove endpoints).'
                USING ERRCODE = 'insufficient_privilege',
                      CONSTRAINT = 'knowledge_edge_endpoints_no_orphan_delete';
        END IF;
        RETURN OLD;
    END IF;
END;
$$;

CREATE TRIGGER knowledge_edge_endpoints_no_update
    BEFORE UPDATE ON knowledge_edge_endpoints
    FOR EACH ROW EXECUTE FUNCTION knowledge_edge_endpoints_reject_write_fn();

CREATE TRIGGER knowledge_edge_endpoints_no_orphan_delete
    BEFORE DELETE ON knowledge_edge_endpoints
    FOR EACH ROW EXECUTE FUNCTION knowledge_edge_endpoints_reject_write_fn();

-- ---------------------------------------------------------------------------
-- knowledge_edge_embeddings
-- ---------------------------------------------------------------------------

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
    FOREIGN KEY (company_id, edge_id)
        REFERENCES knowledge_edges(company_id, id) ON DELETE CASCADE,
    UNIQUE (embedding_model_id, qdrant_point_id)
);

-- ---------------------------------------------------------------------------
-- knowledge_edges_history — temporal audit trail with full lifecycle snapshot
-- ---------------------------------------------------------------------------

CREATE TABLE knowledge_edges_history (
    live_row_id     UUID        NOT NULL,
    edge_id         UUID        NOT NULL,
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
    edge_created_at    TIMESTAMPTZ NOT NULL,
    edge_reviewed_at   TIMESTAMPTZ,
    edge_expires_at    TIMESTAMPTZ,
    edge_superseded_at TIMESTAMPTZ,
    endpoint_0_chunk_id  UUID   NOT NULL,
    endpoint_1_chunk_id  UUID   NOT NULL,
    endpoint_0_role      TEXT   NOT NULL,
    endpoint_1_role      TEXT   NOT NULL,
    change_kind     TEXT        NOT NULL
                        CHECK (change_kind IN ('insert','update','delete')),
    valid_from      TIMESTAMPTZ NOT NULL,
    valid_until     TIMESTAMPTZ,
    CHECK (valid_until IS NULL OR valid_until >= valid_from),
    UNIQUE (live_row_id, valid_from)
);

CREATE UNIQUE INDEX knowledge_edges_history_open_uidx
    ON knowledge_edges_history (live_row_id)
    WHERE valid_until IS NULL;

CREATE INDEX knowledge_edges_history_company_idx
    ON knowledge_edges_history (company_id, valid_from);
CREATE INDEX knowledge_edges_history_endpoint_0_idx
    ON knowledge_edges_history (company_id, endpoint_0_chunk_id);
CREATE INDEX knowledge_edges_history_endpoint_1_idx
    ON knowledge_edges_history (company_id, endpoint_1_chunk_id);

-- ---------------------------------------------------------------------------
-- Trigger-owned snapshot function.
--
-- Not intended for direct call from application code; EXECUTE is REVOKEd
-- from PUBLIC below. Two trigger functions (constraint-trigger for insert,
-- AFTER UPDATE for update) are the only callers; they pass p_kind based on
-- the transition they observed, so callers cannot forge a transition kind.
--
-- Locks the parent edge FOR UPDATE, then captures clock_timestamp() AFTER
-- the lock is acquired (so a snapshot that waited on a concurrent
-- transaction cannot backdate its interval).
-- ---------------------------------------------------------------------------

CREATE OR REPLACE FUNCTION knowledge_edges_snapshot_internal(
    p_edge_id UUID,
    p_kind    TEXT
) RETURNS VOID LANGUAGE plpgsql AS $$
DECLARE
    edge knowledge_edges%ROWTYPE;
    ep0  knowledge_edge_endpoints%ROWTYPE;
    ep1  knowledge_edge_endpoints%ROWTYPE;
    ts   TIMESTAMPTZ;
BEGIN
    IF p_kind NOT IN ('insert','update') THEN
        RAISE EXCEPTION 'knowledge_edges_snapshot_internal: kind must be insert or update, got %',
            p_kind USING ERRCODE = 'invalid_parameter_value';
    END IF;

    -- Lock the parent edge to serialize concurrent snapshot calls.
    SELECT * INTO STRICT edge FROM knowledge_edges
        WHERE id = p_edge_id FOR UPDATE;
    -- Capture timestamp AFTER acquiring the lock so this snapshot's
    -- interval cannot start earlier than the transaction it waited for.
    ts := clock_timestamp();

    SELECT * INTO STRICT ep0 FROM knowledge_edge_endpoints
        WHERE edge_id = p_edge_id AND ordinal = 0;
    SELECT * INTO STRICT ep1 FROM knowledge_edge_endpoints
        WHERE edge_id = p_edge_id AND ordinal = 1;

    UPDATE knowledge_edges_history
        SET valid_until = ts
        WHERE live_row_id = p_edge_id AND valid_until IS NULL;

    INSERT INTO knowledge_edges_history (
        live_row_id, edge_id, company_id, edge_type, direction,
        confidence, origin, review_state, provenance,
        formula_version, edge_version, created_by, reviewed_by,
        edge_created_at, edge_reviewed_at, edge_expires_at, edge_superseded_at,
        endpoint_0_chunk_id, endpoint_1_chunk_id,
        endpoint_0_role, endpoint_1_role,
        change_kind, valid_from, valid_until
    ) VALUES (
        edge.id, edge.id, edge.company_id, edge.edge_type, edge.direction,
        edge.confidence, edge.origin, edge.review_state, edge.provenance,
        edge.formula_version, edge.edge_version, edge.created_by, edge.reviewed_by,
        edge.created_at, edge.reviewed_at, edge.expires_at, edge.superseded_at,
        ep0.chunk_id, ep1.chunk_id,
        ep0.role, ep1.role,
        p_kind, ts, NULL
    );
END;
$$;

-- Prevent direct caller access; only trigger functions call this.
REVOKE EXECUTE ON FUNCTION knowledge_edges_snapshot_internal(UUID, TEXT) FROM PUBLIC;

-- ---------------------------------------------------------------------------
-- Edge-UUID reuse guard: reject INSERT into knowledge_edges if history
-- already exists for that UUID (i.e., the UUID belonged to a previously
-- deleted edge). This closes the P1-2 bypass where the initial-history
-- suppression check would incorrectly skip snapshotting the new edge.
-- ---------------------------------------------------------------------------

CREATE OR REPLACE FUNCTION knowledge_edges_reject_uuid_reuse_fn()
RETURNS TRIGGER LANGUAGE plpgsql AS $$
BEGIN
    IF EXISTS (SELECT 1 FROM knowledge_edges_history WHERE live_row_id = NEW.id) THEN
        RAISE EXCEPTION
            'knowledge_edges UUID % has prior history (deleted edge); UUIDs cannot be reused',
            NEW.id
            USING ERRCODE = 'unique_violation',
                  CONSTRAINT = 'knowledge_edges_no_uuid_reuse';
    END IF;
    RETURN NEW;
END;
$$;

CREATE TRIGGER knowledge_edges_reject_uuid_reuse
    BEFORE INSERT ON knowledge_edges
    FOR EACH ROW EXECUTE FUNCTION knowledge_edges_reject_uuid_reuse_fn();

-- ---------------------------------------------------------------------------
-- CONSTRAINT TRIGGERs: exactly two endpoints at COMMIT.
--
-- Only the parent-table trigger writes the initial-insert history row.
-- The endpoints-table trigger enforces cardinality only.  Because UUID
-- reuse is now blocked by the BEFORE INSERT trigger above, there is no
-- history-suppression race to worry about — every INSERT to knowledge_edges
-- has a clean history slate.
-- ---------------------------------------------------------------------------

CREATE OR REPLACE FUNCTION knowledge_edges_check_count_and_snapshot_fn()
RETURNS TRIGGER LANGUAGE plpgsql AS $$
DECLARE
    v_count INT;
BEGIN
    -- Skip if the edge was deleted in this transaction (cascade path).
    IF NOT EXISTS (SELECT 1 FROM knowledge_edges WHERE id = NEW.id) THEN
        RETURN NEW;
    END IF;

    SELECT count(*) INTO v_count
        FROM knowledge_edge_endpoints WHERE edge_id = NEW.id;
    IF v_count <> 2 THEN
        RAISE EXCEPTION
            'knowledge_edges % must have exactly two endpoints at COMMIT (got %)',
            NEW.id, v_count
            USING ERRCODE = 'check_violation',
                  CONSTRAINT = 'knowledge_edges_exactly_two_endpoints_chk';
    END IF;

    -- Only the parent trigger writes initial history.
    PERFORM knowledge_edges_snapshot_internal(NEW.id, 'insert');
    RETURN NEW;
END;
$$;

CREATE OR REPLACE FUNCTION knowledge_edge_endpoints_check_count_fn()
RETURNS TRIGGER LANGUAGE plpgsql AS $$
DECLARE
    v_edge_id UUID;
    v_count   INT;
BEGIN
    v_edge_id := CASE TG_OP WHEN 'DELETE' THEN OLD.edge_id ELSE NEW.edge_id END;
    -- Skip if the parent edge was deleted in this transaction.
    IF NOT EXISTS (SELECT 1 FROM knowledge_edges WHERE id = v_edge_id) THEN
        RETURN COALESCE(NEW, OLD);
    END IF;
    SELECT count(*) INTO v_count
        FROM knowledge_edge_endpoints WHERE edge_id = v_edge_id;
    IF v_count <> 2 THEN
        RAISE EXCEPTION
            'knowledge_edges % must have exactly two endpoints at COMMIT (got %)',
            v_edge_id, v_count
            USING ERRCODE = 'check_violation',
                  CONSTRAINT = 'knowledge_edges_exactly_two_endpoints_chk';
    END IF;
    RETURN COALESCE(NEW, OLD);
END;
$$;

CREATE CONSTRAINT TRIGGER knowledge_edges_exactly_two_endpoints_parent_trg
    AFTER INSERT ON knowledge_edges
    DEFERRABLE INITIALLY DEFERRED
    FOR EACH ROW EXECUTE FUNCTION knowledge_edges_check_count_and_snapshot_fn();

CREATE CONSTRAINT TRIGGER knowledge_edges_exactly_two_endpoints_trg
    AFTER INSERT OR DELETE ON knowledge_edge_endpoints
    DEFERRABLE INITIALLY DEFERRED
    FOR EACH ROW EXECUTE FUNCTION knowledge_edge_endpoints_check_count_fn();

-- ---------------------------------------------------------------------------
-- AFTER UPDATE ON knowledge_edges — automatic history snapshot.
-- ---------------------------------------------------------------------------

CREATE OR REPLACE FUNCTION knowledge_edges_history_update_fn()
RETURNS TRIGGER LANGUAGE plpgsql AS $$
BEGIN
    PERFORM knowledge_edges_snapshot_internal(NEW.id, 'update');
    RETURN NEW;
END;
$$;

CREATE TRIGGER knowledge_edges_history_after_update
    AFTER UPDATE ON knowledge_edges
    FOR EACH ROW EXECUTE FUNCTION knowledge_edges_history_update_fn();

-- ---------------------------------------------------------------------------
-- BEFORE DELETE ON knowledge_edges — capture endpoints, Qdrant point IDs,
-- and delete marker before CASCADE fires.
-- ---------------------------------------------------------------------------

CREATE OR REPLACE FUNCTION knowledge_edges_history_delete_fn()
RETURNS TRIGGER LANGUAGE plpgsql AS $$
DECLARE
    ep0       knowledge_edge_endpoints%ROWTYPE;
    ep1       knowledge_edge_endpoints%ROWTYPE;
    point_ids UUID[];
    ts        TIMESTAMPTZ := clock_timestamp();
BEGIN
    SELECT * INTO STRICT ep0 FROM knowledge_edge_endpoints
        WHERE edge_id = OLD.id AND ordinal = 0;
    SELECT * INTO STRICT ep1 FROM knowledge_edge_endpoints
        WHERE edge_id = OLD.id AND ordinal = 1;

    SELECT array_agg(qdrant_point_id) INTO point_ids
        FROM knowledge_edge_embeddings WHERE edge_id = OLD.id;

    -- Emit outbox event for Qdrant cleanup. A scheduler consumer for
    -- job_type='qdrant_delete_edge_points' ships in a follow-up PR alongside
    -- the edge-vector indexing worker. Retrieval is safe in the interim
    -- because EvidenceGate revalidates on live PG state.
    IF point_ids IS NOT NULL AND array_length(point_ids, 1) > 0 THEN
        INSERT INTO outbox_events (
            company_id, aggregate_id, job_type, job_schema_version,
            payload, idempotency_key
        ) VALUES (
            OLD.company_id, OLD.id, 'qdrant_delete_edge_points', 1,
            jsonb_build_object(
                'edge_id', OLD.id,
                'qdrant_point_ids', to_jsonb(point_ids)),
            'edge-del:' || OLD.id::text || ':' || OLD.edge_version::text
        );
    END IF;

    UPDATE knowledge_edges_history
        SET valid_until = ts
        WHERE live_row_id = OLD.id AND valid_until IS NULL;

    INSERT INTO knowledge_edges_history (
        live_row_id, edge_id, company_id, edge_type, direction,
        confidence, origin, review_state, provenance,
        formula_version, edge_version, created_by, reviewed_by,
        edge_created_at, edge_reviewed_at, edge_expires_at, edge_superseded_at,
        endpoint_0_chunk_id, endpoint_1_chunk_id,
        endpoint_0_role, endpoint_1_role,
        change_kind, valid_from, valid_until
    ) VALUES (
        OLD.id, OLD.id, OLD.company_id, OLD.edge_type, OLD.direction,
        OLD.confidence, OLD.origin, OLD.review_state, OLD.provenance,
        OLD.formula_version, OLD.edge_version, OLD.created_by, OLD.reviewed_by,
        OLD.created_at, OLD.reviewed_at, OLD.expires_at, OLD.superseded_at,
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

COMMENT ON TABLE knowledge_edges IS
    'BaryGraph Lite chunk-to-chunk semantic edges. See docs/barygraph_features.md.';
