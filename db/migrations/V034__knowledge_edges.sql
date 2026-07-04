-- V034: BaryGraph Lite knowledge edges (chunk-to-chunk semantic relationships)
--
-- First of three BaryGraph Lite migrations (see docs/barygraph_features.md).
-- V035 adds capability grants; V036 adds privileged access sessions.
--
-- Correctness invariants enforced at the database level:
--
--   * Zero-endpoint edges cannot commit.  Two DEFERRABLE INITIALLY DEFERRED
--     CONSTRAINT TRIGGERs — one on the parent table, one on the endpoints
--     table — both verify count(*)=2 at COMMIT.
--
--   * Endpoints are immutable.  UPDATE on knowledge_edge_endpoints is
--     rejected by trigger.  Endpoint identity changes require deleting the
--     edge and recreating it, matching the "edges are superseded not
--     modified" design principle.
--
--   * Same-company actors enforced by composite foreign keys (not a BEFORE
--     trigger, which had a concurrency race under a user company_id
--     UPDATE).  ON DELETE NO ACTION because Wikore soft-deletes users via
--     deactivated_at; hard-delete of a user referenced by an edge requires
--     explicit cleanup by the caller.
--
--   * History is written by triggers, not by convention.  AFTER UPDATE ON
--     knowledge_edges fires the shared snapshot procedure automatically,
--     so direct SQL and repair scripts cannot silently lose history.
--
--   * knowledge_edges_snapshot() locks the parent edge FOR UPDATE, generates
--     clock_timestamp() internally (not from caller), and enforces
--     valid_from < valid_until.
--
--   * DELETE trigger uses SELECT INTO STRICT — an edge whose endpoints have
--     been removed independently (which is only possible via the endpoint
--     table, and only if constraint trigger was disabled) fails closed
--     rather than fabricating a zero-UUID history record.
--
--   * The BEFORE DELETE trigger emits a qdrant_delete_edge_points outbox
--     event carrying the point ID array before cascade removes embedding
--     rows.  A scheduler consumer for this job type ships in a follow-up
--     PR alongside the edge-vector indexing worker.

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
    -- Actor same-company enforced by composite FKs below (not by trigger,
    -- which had a concurrency race under a users.company_id UPDATE).
    created_by          UUID,
    reviewed_by         UUID,
    created_at          TIMESTAMPTZ NOT NULL DEFAULT now(),
    reviewed_at         TIMESTAMPTZ,
    expires_at          TIMESTAMPTZ,
    superseded_at       TIMESTAMPTZ,
    CHECK (expires_at IS NULL OR expires_at > created_at),
    -- Required as composite-FK target for endpoints/embeddings child tables.
    UNIQUE (company_id, id),
    -- Composite FKs for actor same-company (PG17: NO ACTION so a user
    -- hard-delete surfaces as a FK violation the caller must resolve;
    -- deactivate_at is the normal user-removal path in Wikore).
    CONSTRAINT knowledge_edges_created_by_same_company_fk
        FOREIGN KEY (company_id, created_by)
        REFERENCES users(company_id, id) MATCH SIMPLE
        ON DELETE NO ACTION,
    CONSTRAINT knowledge_edges_reviewed_by_same_company_fk
        FOREIGN KEY (company_id, reviewed_by)
        REFERENCES users(company_id, id) MATCH SIMPLE
        ON DELETE NO ACTION
);

-- V1 chunk-to-chunk edge types only. supersedes, section_parent_of,
-- section_contains, responsible_team require non-chunk endpoints (V2).
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

-- Endpoints are immutable. Changing an endpoint requires deleting the edge
-- and recreating it — matching the "edges are superseded not modified"
-- design principle. Reject UPDATE entirely.
CREATE OR REPLACE FUNCTION knowledge_edge_endpoints_immutable_fn()
RETURNS TRIGGER LANGUAGE plpgsql AS $$
BEGIN
    RAISE EXCEPTION
        'knowledge_edge_endpoints is immutable; UPDATE rejected. Delete and recreate the edge instead.'
        USING ERRCODE = 'insufficient_privilege',
              CONSTRAINT = 'knowledge_edge_endpoints_immutable';
END;
$$;

CREATE TRIGGER knowledge_edge_endpoints_no_update
    BEFORE UPDATE ON knowledge_edge_endpoints
    FOR EACH ROW EXECUTE FUNCTION knowledge_edge_endpoints_immutable_fn();

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
    -- Lifecycle timestamps — required for as-of EvidenceGate reconstruction
    -- (was this edge usable at time T?). Captured from the parent row.
    edge_created_at    TIMESTAMPTZ NOT NULL,
    edge_reviewed_at   TIMESTAMPTZ,
    edge_expires_at    TIMESTAMPTZ,
    edge_superseded_at TIMESTAMPTZ,
    -- Endpoint snapshot: captured from knowledge_edge_endpoints BEFORE
    -- CASCADE deletes them. Both chunk UUIDs are required.
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

-- One open interval per edge at most.
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
-- Shared history snapshot procedure — used by INSERT (CONSTRAINT TRIGGER at
-- COMMIT) and UPDATE (AFTER UPDATE trigger) paths.
--
-- Locks the parent edge FOR UPDATE to serialize concurrent snapshot calls.
-- Generates the timestamp internally (not from caller) so audit records
-- always reflect the actual DB clock at commit time.
-- ---------------------------------------------------------------------------

CREATE OR REPLACE PROCEDURE knowledge_edges_snapshot_at(
    p_edge_id UUID,
    p_kind    TEXT
) LANGUAGE plpgsql AS $$
DECLARE
    edge knowledge_edges%ROWTYPE;
    ep0  knowledge_edge_endpoints%ROWTYPE;
    ep1  knowledge_edge_endpoints%ROWTYPE;
    ts   TIMESTAMPTZ := clock_timestamp();
BEGIN
    IF p_kind NOT IN ('insert','update') THEN
        RAISE EXCEPTION 'knowledge_edges_snapshot_at: kind must be insert or update, got %',
            p_kind USING ERRCODE = 'invalid_parameter_value';
    END IF;

    -- Lock the parent edge to serialize concurrent snapshot calls.
    SELECT * INTO STRICT edge FROM knowledge_edges
        WHERE id = p_edge_id FOR UPDATE;
    SELECT * INTO STRICT ep0  FROM knowledge_edge_endpoints
        WHERE edge_id = p_edge_id AND ordinal = 0;
    SELECT * INTO STRICT ep1  FROM knowledge_edge_endpoints
        WHERE edge_id = p_edge_id AND ordinal = 1;

    -- Close the currently-open interval, then insert a new open row.
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

-- ---------------------------------------------------------------------------
-- CONSTRAINT TRIGGER: exactly two endpoints at COMMIT + initial history row.
--
-- Fires at COMMIT (not per-INSERT), so:
--   * The two-endpoint invariant catches BOTH the "zero endpoints" case
--     (edge inserted with no endpoint rows at all) AND the "one endpoint"
--     case, via a single check.
--   * The history "insert" row is written after both endpoints are known,
--     eliminating the insertion-order dependency (ordinal=1 before ordinal=0
--     is fine — both must be present at COMMIT).
--
-- Two triggers point to this function: one on the parent (catches the
-- zero-endpoint case), one on the endpoints table (catches the one-endpoint
-- case).  Both DEFERRABLE INITIALLY DEFERRED so the check runs once at COMMIT.
-- ---------------------------------------------------------------------------

CREATE OR REPLACE FUNCTION knowledge_edges_check_endpoint_count_fn()
RETURNS TRIGGER LANGUAGE plpgsql AS $$
DECLARE
    v_edge_id UUID;
    v_count   INT;
    v_has_history BOOLEAN;
BEGIN
    -- Locate the edge id from either NEW (parent INSERT) or OLD (endpoint DELETE).
    v_edge_id := COALESCE(
        (TG_TABLE_NAME = 'knowledge_edges' AND TG_OP <> 'DELETE') :: INT * 0 + NULL,
        NULL
    );
    IF TG_TABLE_NAME = 'knowledge_edges' THEN
        v_edge_id := CASE TG_OP WHEN 'DELETE' THEN OLD.id ELSE NEW.id END;
    ELSE
        v_edge_id := CASE TG_OP WHEN 'DELETE' THEN OLD.edge_id ELSE NEW.edge_id END;
    END IF;

    -- Skip if the parent edge was deleted in this transaction (cascade case).
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

    -- Write the initial history row exactly once per edge.
    -- Called from BOTH triggers; the second call finds the row already
    -- present via the open-interval partial unique index and skips.
    SELECT EXISTS (
        SELECT 1 FROM knowledge_edges_history
        WHERE live_row_id = v_edge_id
    ) INTO v_has_history;

    IF NOT v_has_history THEN
        CALL knowledge_edges_snapshot_at(v_edge_id, 'insert');
    END IF;

    RETURN COALESCE(NEW, OLD);
END;
$$;

-- Parent-table trigger: catches the "zero endpoints" case (edge inserted
-- with no endpoint rows). Fires AFTER INSERT so knowledge_edges row exists
-- when the check runs at COMMIT.
CREATE CONSTRAINT TRIGGER knowledge_edges_exactly_two_endpoints_parent_trg
    AFTER INSERT ON knowledge_edges
    DEFERRABLE INITIALLY DEFERRED
    FOR EACH ROW EXECUTE FUNCTION knowledge_edges_check_endpoint_count_fn();

-- Endpoints-table trigger: catches all other transitions (one endpoint,
-- endpoint delete without cascade, etc.)
CREATE CONSTRAINT TRIGGER knowledge_edges_exactly_two_endpoints_trg
    AFTER INSERT OR DELETE ON knowledge_edge_endpoints
    DEFERRABLE INITIALLY DEFERRED
    FOR EACH ROW EXECUTE FUNCTION knowledge_edges_check_endpoint_count_fn();

-- ---------------------------------------------------------------------------
-- AFTER UPDATE ON knowledge_edges — automatic history snapshot.
--
-- Fires on every UPDATE (application, psql, repair scripts). Missing a
-- history call is not possible with this trigger installed.
-- ---------------------------------------------------------------------------

CREATE OR REPLACE FUNCTION knowledge_edges_history_update_fn()
RETURNS TRIGGER LANGUAGE plpgsql AS $$
BEGIN
    CALL knowledge_edges_snapshot_at(NEW.id, 'update');
    RETURN NEW;
END;
$$;

CREATE TRIGGER knowledge_edges_history_after_update
    AFTER UPDATE ON knowledge_edges
    FOR EACH ROW EXECUTE FUNCTION knowledge_edges_history_update_fn();

-- ---------------------------------------------------------------------------
-- BEFORE DELETE ON knowledge_edges — capture endpoints, Qdrant point IDs,
-- and delete marker before CASCADE fires. SELECT INTO STRICT fails closed
-- if endpoints have already been removed (fabricating audit is not allowed).
-- ---------------------------------------------------------------------------

CREATE OR REPLACE FUNCTION knowledge_edges_history_delete_fn()
RETURNS TRIGGER LANGUAGE plpgsql AS $$
DECLARE
    ep0       knowledge_edge_endpoints%ROWTYPE;
    ep1       knowledge_edge_endpoints%ROWTYPE;
    point_ids UUID[];
    ts        TIMESTAMPTZ := clock_timestamp();
BEGIN
    -- Capture endpoints BEFORE cascade removes them. STRICT: fail closed
    -- if either endpoint is missing (do not fabricate a zero-UUID history).
    SELECT * INTO STRICT ep0 FROM knowledge_edge_endpoints
        WHERE edge_id = OLD.id AND ordinal = 0;
    SELECT * INTO STRICT ep1 FROM knowledge_edge_endpoints
        WHERE edge_id = OLD.id AND ordinal = 1;

    -- Capture Qdrant point IDs BEFORE cascade removes embedding rows.
    SELECT array_agg(qdrant_point_id) INTO point_ids
        FROM knowledge_edge_embeddings WHERE edge_id = OLD.id;

    -- Emit outbox event for Qdrant cleanup. A scheduler consumer for job_type
    -- = 'qdrant_delete_edge_points' ships in a follow-up PR alongside the
    -- edge-vector indexing worker; until then, deleted edge points remain
    -- in Qdrant. That is safe because retrieval always passes through
    -- EvidenceGate on the live PG state — an orphan Qdrant point cannot
    -- surface as evidence.
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

    -- Close the currently-open interval.
    UPDATE knowledge_edges_history
        SET valid_until = ts
        WHERE live_row_id = OLD.id AND valid_until IS NULL;

    -- Insert the delete marker (zero-width interval, valid_from = valid_until).
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
