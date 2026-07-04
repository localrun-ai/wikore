-- V034: BaryGraph Lite knowledge edges (chunk-to-chunk semantic relationships)
--
-- First phase of BaryGraph Lite (see docs/barygraph_features.md). V034 adds
-- only the edge data model. Capabilities/entitlements ship in V035;
-- privileged-access sessions ship in V036. The three tables are independent
-- and can be reviewed/rolled back separately.
--
-- V1 scope:
--   * Chunk-to-chunk edges only. Non-chunk endpoints (section, version,
--     org_unit) are V2 and blocked by knowledge_edges_edge_type_v1_chk.
--   * Explicit admin-authored edges. No automated parser/LLM edge creation
--     in this migration; those arrive in later phases.
--
-- Design invariants:
--   * Directed multigraph. cycle detection is the caller's responsibility;
--     acyclicity is not enforced at the schema level (contradicts is
--     symmetric, procedure dependency chains can be circular in practice).
--   * Tenant boundary is enforced at the database level via composite FKs
--     (company_id, id) — see UNIQUE(company_id, id) below.
--   * Exactly-two-endpoints enforced by a DEFERRABLE INITIALLY DEFERRED
--     CONSTRAINT TRIGGER that fires at COMMIT (not per-INSERT), so the
--     repository can insert both endpoints in one transaction without
--     tripping the check on the first insert.
--   * Every hard-delete of a knowledge_edges row emits an outbox event
--     carrying the Qdrant point IDs BEFORE cascade removes the embedding
--     rows. This makes direct-DELETE from psql safe (DBA incident response,
--     repair jobs) without relying on application-layer discipline.
--   * knowledge_edges_history captures every INSERT and DELETE with an
--     endpoint snapshot (both chunk UUIDs). UPDATE snapshots are the
--     repository's responsibility via knowledge_edges_snapshot(...) — no
--     AFTER UPDATE trigger, because trigger composition across two tables
--     produces unpredictable ordering that is difficult to reason about.

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
    created_by          UUID REFERENCES users(id) ON DELETE SET NULL,
    reviewed_by         UUID REFERENCES users(id) ON DELETE SET NULL,
    created_at          TIMESTAMPTZ NOT NULL DEFAULT now(),
    reviewed_at         TIMESTAMPTZ,
    expires_at          TIMESTAMPTZ,
    superseded_at       TIMESTAMPTZ,
    CHECK (expires_at IS NULL OR expires_at > created_at),
    -- Required as composite-FK target for child tables (endpoints, embeddings).
    UNIQUE (company_id, id)
);

-- V1 chunk-to-chunk edge types only.
-- supersedes, section_parent_of, section_contains, responsible_team require
-- non-chunk endpoints and are not insertable until V2 typed endpoints land.
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

-- Enforce same-company created_by / reviewed_by. Composite FKs on the users
-- table are not usable here because ON DELETE SET NULL requires a single-column
-- FK (Wikore uses this pattern in documents, wiki_page_versions, etc.).
CREATE OR REPLACE FUNCTION knowledge_edges_actors_same_company()
RETURNS TRIGGER LANGUAGE plpgsql AS $$
BEGIN
    IF NEW.created_by IS NOT NULL AND NOT EXISTS (
        SELECT 1 FROM users
        WHERE id = NEW.created_by AND company_id = NEW.company_id
    ) THEN
        RAISE EXCEPTION 'knowledge_edges.created_by must belong to company %',
            NEW.company_id
            USING ERRCODE = 'foreign_key_violation',
                  CONSTRAINT = 'knowledge_edges_created_by_same_company_fk';
    END IF;
    IF NEW.reviewed_by IS NOT NULL AND NOT EXISTS (
        SELECT 1 FROM users
        WHERE id = NEW.reviewed_by AND company_id = NEW.company_id
    ) THEN
        RAISE EXCEPTION 'knowledge_edges.reviewed_by must belong to company %',
            NEW.company_id
            USING ERRCODE = 'foreign_key_violation',
                  CONSTRAINT = 'knowledge_edges_reviewed_by_same_company_fk';
    END IF;
    RETURN NEW;
END;
$$;

CREATE TRIGGER knowledge_edges_actors_same_company_trg
    BEFORE INSERT OR UPDATE ON knowledge_edges
    FOR EACH ROW EXECUTE FUNCTION knowledge_edges_actors_same_company();

-- ---------------------------------------------------------------------------
-- knowledge_edge_endpoints (chunk-to-chunk only in V1)
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
    -- Composite FK enforces company_id matches the parent edge.
    FOREIGN KEY (company_id, edge_id)
        REFERENCES knowledge_edges(company_id, id) ON DELETE CASCADE,
    FOREIGN KEY (company_id, chunk_id)
        REFERENCES document_chunks(company_id, id) ON DELETE RESTRICT
);

-- Required for "find all edges referencing this chunk" queries: deletion
-- workflow, resync worker, impact analysis. Without this index,
-- WHERE chunk_id = $1 is a full table scan on every chunk delete.
CREATE INDEX knowledge_edge_endpoints_chunk_idx
    ON knowledge_edge_endpoints (company_id, chunk_id);

-- Two-endpoint CONSTRAINT TRIGGER. A regular CREATE TRIGGER cannot be
-- deferred; only CREATE CONSTRAINT TRIGGER DEFERRABLE INITIALLY DEFERRED
-- can. Fires at COMMIT (not per-INSERT), so the repository can insert both
-- endpoints in one transaction before the count is checked.
CREATE OR REPLACE FUNCTION knowledge_edges_check_endpoint_count_fn()
RETURNS TRIGGER LANGUAGE plpgsql AS $$
DECLARE
    v_edge_id UUID;
BEGIN
    -- NEW is unassigned in a DELETE trigger; dispatch on TG_OP.
    v_edge_id := CASE TG_OP WHEN 'DELETE' THEN OLD.edge_id ELSE NEW.edge_id END;

    -- Skip edges deleted in this same transaction (cascade case).
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
-- knowledge_edges_history — temporal audit trail
-- ---------------------------------------------------------------------------

CREATE TABLE knowledge_edges_history (
    -- Plain UUID, no FK — live_row_id becomes stale after the knowledge_edges
    -- row is deleted, matching memberships_history (V024).
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
    -- Endpoint snapshot: captured from knowledge_edge_endpoints BEFORE
    -- CASCADE deletes them. Both chunk UUIDs are required — without them
    -- the history record cannot answer "which chunks were connected."
    endpoint_0_chunk_id  UUID   NOT NULL,
    endpoint_1_chunk_id  UUID   NOT NULL,
    endpoint_0_role      TEXT   NOT NULL,
    endpoint_1_role      TEXT   NOT NULL,
    change_kind     TEXT        NOT NULL
                        CHECK (change_kind IN ('insert','update','delete')),
    valid_from      TIMESTAMPTZ NOT NULL,
    valid_until     TIMESTAMPTZ,
    UNIQUE (live_row_id, valid_from)
);

-- One open interval per edge at most — matches memberships_history pattern.
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
-- Shared history snapshot procedure — called by the repository after any
-- edge/endpoint update. Not a trigger function. AFTER UPDATE triggers on
-- knowledge_edges / knowledge_edge_endpoints are NOT installed because
-- trigger composition across the two tables produces ordering that is
-- difficult to reason about; explicit repository control is preferred.
-- ---------------------------------------------------------------------------

CREATE OR REPLACE PROCEDURE knowledge_edges_snapshot(
    p_edge_id UUID,
    p_kind    TEXT,
    p_ts      TIMESTAMPTZ
) LANGUAGE plpgsql AS $$
DECLARE
    edge knowledge_edges%ROWTYPE;
    ep0  knowledge_edge_endpoints%ROWTYPE;
    ep1  knowledge_edge_endpoints%ROWTYPE;
BEGIN
    SELECT * INTO STRICT edge FROM knowledge_edges WHERE id = p_edge_id;
    SELECT * INTO STRICT ep0  FROM knowledge_edge_endpoints
        WHERE edge_id = p_edge_id AND ordinal = 0;
    SELECT * INTO STRICT ep1  FROM knowledge_edge_endpoints
        WHERE edge_id = p_edge_id AND ordinal = 1;

    -- Close the currently-open interval, then insert a new open row.
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

-- ---------------------------------------------------------------------------
-- AFTER INSERT ordinal=1 — snapshot the new edge to history.
-- Fires only on ordinal=1 because by the repository invariant ordinal=0 is
-- inserted first. SELECT INTO STRICT surfaces the ordering violation with a
-- clean error message rather than silently NULLing endpoint_0_chunk_id.
-- ---------------------------------------------------------------------------

CREATE OR REPLACE FUNCTION knowledge_edge_endpoints_history_insert_fn()
RETURNS TRIGGER LANGUAGE plpgsql AS $$
DECLARE
    edge knowledge_edges%ROWTYPE;
    ep0  knowledge_edge_endpoints%ROWTYPE;
    ts   TIMESTAMPTZ := clock_timestamp();
BEGIN
    IF NEW.ordinal <> 1 THEN RETURN NEW; END IF;

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

-- ---------------------------------------------------------------------------
-- BEFORE DELETE on knowledge_edges — captures endpoints and Qdrant point IDs
-- BEFORE cascade removes them, emits outbox event for Qdrant cleanup, and
-- writes the delete marker to history. Safe from any caller: application,
-- psql, repair jobs.
-- ---------------------------------------------------------------------------

CREATE OR REPLACE FUNCTION knowledge_edges_history_delete_fn()
RETURNS TRIGGER LANGUAGE plpgsql AS $$
DECLARE
    ep0       knowledge_edge_endpoints%ROWTYPE;
    ep1       knowledge_edge_endpoints%ROWTYPE;
    point_ids UUID[];
    ts        TIMESTAMPTZ := clock_timestamp();
BEGIN
    -- Capture endpoints BEFORE cascade removes them.
    SELECT * INTO ep0 FROM knowledge_edge_endpoints
        WHERE edge_id = OLD.id AND ordinal = 0;
    SELECT * INTO ep1 FROM knowledge_edge_endpoints
        WHERE edge_id = OLD.id AND ordinal = 1;

    -- Capture Qdrant point IDs BEFORE cascade removes embedding rows.
    SELECT array_agg(qdrant_point_id) INTO point_ids
        FROM knowledge_edge_embeddings WHERE edge_id = OLD.id;

    -- Emit outbox event for Qdrant cleanup. Column names from V015:
    -- job_type (not event_type), idempotency_key NOT NULL.
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
        COALESCE(ep0.chunk_id, '00000000-0000-0000-0000-000000000000'::uuid),
        COALESCE(ep1.chunk_id, '00000000-0000-0000-0000-000000000000'::uuid),
        COALESCE(ep0.role, ''),
        COALESCE(ep1.role, ''),
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
