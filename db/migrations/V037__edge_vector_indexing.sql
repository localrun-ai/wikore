-- V037: BaryGraph Lite edge vector indexing infrastructure
--
-- Fifth of the BaryGraph Lite series (V034 edges, V035 capabilities,
-- V036 privileged access, and PR #53's outbox consumer for the delete
-- side). This migration adds the WRITE-side infrastructure:
--
--   * edge_type_vectors — a per-formula-version, per-embedding-model
--     registry of the semantic vector for each edge_type. Populated
--     ONCE by an admin indexing job that embeds the type descriptions
--     from kTypeDescriptions in the source; the scheduler workers read
--     the pre-computed vectors, never regenerate them.
--
--   * qdrant_upsert_edge_vector — a new outbox job_type emitted by
--     an AFTER INSERT / AFTER UPDATE trigger on knowledge_edges. The
--     scheduler consumer (ships in the follow-up PR) reads the row,
--     joins the endpoints and their per-chunk vectors, combines them
--     via formula v1 with the type vector, and upserts to Qdrant.
--
-- The design doc's Wikore-adapted formula (§"Wikore-adapted formula
-- (formula_version = 1)") requires:
--   * two endpoint chunk vectors, weighted by an authority ratio
--     computed from documents.authority_level with a floor of 10
--   * one type vector from edge_type_vectors keyed by
--     (formula_version, edge_type, embedding_model_id)
--   * fixed weights w_ep = 0.65, w_type = 0.35 for formula_version = 1
--
-- Point ID stability follows the doc's uuid_v5 spec:
--   uuid_v5(edge_id + ':' + embedding_model_id + ':' + formula_version
--         + ':' + edge_version)
-- The scheduler is responsible for computing this; the trigger only
-- names which edge_version is authoritative at the enqueue moment.
--
-- Every edge INSERT (via the parent CONSTRAINT TRIGGER path from V034)
-- and every UPDATE that bumps edge_version enqueues one upsert event
-- per enabled embedding_model. Retrieval-safety before the worker runs
-- is preserved because EvidenceGate never trusts Qdrant for
-- authorization — a stale (or missing) point is a recall issue, not a
-- leak.

-- ---------------------------------------------------------------------------
-- edge_type_vectors — per-(formula_version, edge_type, embedding_model)
--                     registry of the semantic vector used by the edge
--                     vector formula.
-- ---------------------------------------------------------------------------
--
-- Populated ONCE per formula version by the admin
-- EmbedTypeVectorsUseCase (ships alongside the worker). Reads are
-- keyed on (formula_version, edge_type, embedding_model_id) which is
-- the invariant the worker needs. Vectors are stored as REAL[] so
-- Postgres remains model-agnostic (Wikore does not use pgvector; the
-- production ANN search is in Qdrant, PG only carries the pre-computed
-- constant here).
--
-- edge_type spans BOTH V1 (chunk-to-chunk) and future V2 types so this
-- table doesn't need a schema change when v2 types are activated by a
-- widening of knowledge_edges_edge_type_v1_chk. A row for a currently-
-- disallowed edge_type is harmless — no edge references it yet.

CREATE TABLE edge_type_vectors (
    formula_version    INT         NOT NULL,
    edge_type          TEXT        NOT NULL,
    embedding_model_id UUID        NOT NULL
                             REFERENCES embedding_models(id) ON DELETE RESTRICT,
    vector             REAL[]      NOT NULL,
    -- The exact source string that produced this vector, kept alongside
    -- the vector so a diff of kTypeDescriptions between formula versions
    -- is visible without re-embedding. Length-limited to keep the row
    -- small; long descriptions are a smell that should live elsewhere.
    description        TEXT        NOT NULL CHECK (length(description) BETWEEN 1 AND 512),
    embedded_at        TIMESTAMPTZ NOT NULL DEFAULT now(),
    -- edge_version of the formula constant itself; bumped whenever the
    -- admin job re-embeds a description (edits are rare but possible
    -- during pre-v2 tuning). The worker uses (formula_version,
    -- edge_type) alone; this column is diagnostic.
    revision           INT         NOT NULL DEFAULT 1,
    PRIMARY KEY (formula_version, edge_type, embedding_model_id),
    CONSTRAINT edge_type_vectors_vector_dim_positive_chk
        CHECK (array_length(vector, 1) IS NOT NULL AND array_length(vector, 1) > 0)
);

COMMENT ON TABLE edge_type_vectors IS
    'Per-formula-version type-vector registry. Populated by the admin '
    'embed-type-vectors job. Read by the edge-vector worker. '
    'See docs/barygraph_features.md §"Relation-type vectors".';

CREATE INDEX edge_type_vectors_lookup_idx
    ON edge_type_vectors (formula_version, embedding_model_id, edge_type);

-- ---------------------------------------------------------------------------
-- Outbox emission on edge write.
--
-- Fires an outbox event carrying enough information for the
-- EmbedEdgeWorker to compute and upsert the edge vector without a
-- second round-trip to knowledge_edges. Every enabled embedding model
-- gets its own event so the worker can process each model's collection
-- independently; disabled models are skipped.
-- ---------------------------------------------------------------------------

CREATE OR REPLACE FUNCTION knowledge_edges_enqueue_upsert_fn()
RETURNS TRIGGER LANGUAGE plpgsql
-- SECURITY DEFINER + explicit search_path — same rationale as V034's
-- knowledge_edges_check_count_and_snapshot_fn: keep the outbox INSERT
-- reachable through the trigger regardless of any future split between
-- migration and app roles, and pin the search_path so an object of the
-- same name in a caller-controlled schema cannot shadow public.
SECURITY DEFINER
SET search_path = pg_catalog, public
AS $$
DECLARE
    model_id UUID;
BEGIN
    -- On UPDATE, only enqueue when edge_version actually advanced. The
    -- AFTER UPDATE history trigger (V034) also fires on any UPDATE,
    -- but the vector only needs re-computing when edge_version bumps,
    -- and repository UPDATEs bump edge_version whenever ANY meaningful
    -- field changes.
    IF TG_OP = 'UPDATE' AND NEW.edge_version = OLD.edge_version THEN
        RETURN NEW;
    END IF;

    -- One event per enabled model. The scheduler worker computes the
    -- point ID from (edge_id, embedding_model_id, formula_version,
    -- edge_version) so a redelivered event upserts the SAME point ID
    -- and is idempotent against Qdrant.
    FOR model_id IN
        SELECT id FROM embedding_models WHERE enabled = true
    LOOP
        INSERT INTO outbox_events (
            company_id, aggregate_id, job_type, job_schema_version,
            payload, idempotency_key
        ) VALUES (
            NEW.company_id, NEW.id, 'qdrant_upsert_edge_vector', 1,
            jsonb_build_object(
                'edge_id',            NEW.id,
                'edge_type',          NEW.edge_type,
                'formula_version',    NEW.formula_version,
                'edge_version',       NEW.edge_version,
                'embedding_model_id', model_id,
                'review_state',       NEW.review_state),
            -- Include model id so per-model events are independently
            -- claimable AND idempotency_key per (edge, model, version)
            -- collapses same-version retriggers.
            'edge-upsert:' || NEW.id::text || ':' || model_id::text
                          || ':' || NEW.edge_version::text
        )
        ON CONFLICT (company_id, job_type, idempotency_key) DO NOTHING;
    END LOOP;
    RETURN NEW;
END;
$$;

-- Trigger fires AFTER INSERT/UPDATE and reads freshly-committed state.
-- The V034 parent CONSTRAINT TRIGGER (which asserts two endpoints and
-- writes initial history) runs at COMMIT via DEFERRABLE INITIALLY
-- DEFERRED, so this AFTER INSERT sees a valid edge with endpoints
-- present.
CREATE TRIGGER knowledge_edges_enqueue_upsert_insert
    AFTER INSERT ON knowledge_edges
    FOR EACH ROW EXECUTE FUNCTION knowledge_edges_enqueue_upsert_fn();

CREATE TRIGGER knowledge_edges_enqueue_upsert_update
    AFTER UPDATE ON knowledge_edges
    FOR EACH ROW EXECUTE FUNCTION knowledge_edges_enqueue_upsert_fn();

-- No permissions grant needed on the function beyond default (owner
-- executes it; the trigger runs as SECURITY DEFINER against the owner).
-- REVOKE from public so application code cannot call it directly to
-- forge events, matching the pattern established for
-- knowledge_edges_snapshot_internal in V034.
REVOKE EXECUTE ON FUNCTION knowledge_edges_enqueue_upsert_fn() FROM PUBLIC;

COMMENT ON FUNCTION knowledge_edges_enqueue_upsert_fn() IS
    'Enqueues a qdrant_upsert_edge_vector outbox event per enabled '
    'embedding model on knowledge_edges INSERT (and on UPDATE that '
    'bumps edge_version). Consumed by the EmbedEdgeWorker scheduler.';
