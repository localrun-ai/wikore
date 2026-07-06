-- V038: Shared visibility function for G1 and G2 evidence gates.
--
-- Consolidates the three visibility arms that EvidenceGate (G1) and
-- RelationshipEvidenceGate (G2) previously carried as identical
-- copies. Both gates now call this function, so a drift between them
-- becomes structurally impossible — the earlier "MUST stay in
-- lock-step" comment in G2 (and its equivalent inline duplication
-- in G1) is no longer a standing security-drift risk.
--
-- Design:
--   Given a caller's reader_scope (the org_unit_ids of their
--   membership, potentially with descendants applied) and a set of
--   candidate document IDs, return the subset the caller may access.
--
-- Access is granted if ANY of these arms admits the doc:
--
--   Arm 1 — Ownership: the document's owner_org_unit_id is in the
--           reader's scope. Direct ownership; no grants involved.
--
--   Arm 2 — Direct document grant: an unexpired resource_grant on
--           this specific document exists to a principal that
--           matches the reader by principal_applies_to semantics:
--             * self_only: principal_id is in reader_scope directly.
--             * self_and_descendants: principal_id is an ancestor
--               (or self) of any reader_scope entry. reader_grant_keys
--               below folds the closure in.
--
--   Arm 3 — Org-unit grant with closure: a grant on an org_unit that
--           is an ancestor (via org_unit_closure) of the document's
--           owner. resource_applies_to=self_only requires the grant's
--           org_unit to BE the document's owner (depth=0); the
--           default self_and_descendants covers the whole subtree.
--
-- Lifecycle and sensitivity filtering are NOT the function's job;
-- the caller applies those on the join to document_chunks /
-- document_versions before deciding what to feed as
-- p_candidate_doc_ids. This keeps the function focused on ACCESS
-- and independent of the per-gate hydration shape.
--
-- Volatility: STABLE. The function reads resource_grants, org_unit_closure,
-- and documents — all of which may change between transactions but not
-- within one. PARALLEL SAFE so a query planner can parallelise over
-- the arms.
--
-- Callers: EvidenceGate (chunk hydration), RelationshipEvidenceGate
-- (endpoint hydration). Any future gate over document-scoped resources
-- should call this and NOT reimplement the arms inline.

CREATE OR REPLACE FUNCTION wikore_visible_doc_ids(
    p_company_id        uuid,
    p_reader_scope      uuid[],
    p_candidate_doc_ids uuid[]
) RETURNS TABLE (doc_id uuid)
LANGUAGE sql
STABLE
PARALLEL SAFE
AS $function$
    WITH reader_scope(ou_id) AS (
        SELECT DISTINCT x FROM unnest(p_reader_scope) AS x
    ),
    reader_grant_keys AS (
        -- reader_scope plus every ancestor of any scope entry — the
        -- set of org_unit_ids that a self_and_descendants grant may
        -- attach to and still reach the reader.
        SELECT ou_id FROM reader_scope
        UNION
        SELECT c.ancestor_id
        FROM   org_unit_closure c
        WHERE  c.company_id  = p_company_id
          AND  c.descendant_id IN (SELECT ou_id FROM reader_scope)
    ),
    cand AS (
        SELECT d.id AS doc_id, d.owner_org_unit_id AS owner
        FROM   documents d
        WHERE  d.company_id = p_company_id
          AND  d.id         = ANY(p_candidate_doc_ids)
    )
    -- Arm 1: ownership.
    SELECT doc_id FROM cand
    WHERE  owner IN (SELECT ou_id FROM reader_scope)
  UNION
    -- Arm 2: direct document grant.
    SELECT c.doc_id FROM cand c
    JOIN   resource_grants rg
        ON rg.company_id    = p_company_id
       AND rg.resource_type = 'document'
       AND rg.resource_id   = c.doc_id
       AND rg.permission IN ('read','write','admin')
       AND (rg.expires_at IS NULL OR rg.expires_at > now())
    WHERE  (rg.principal_applies_to = 'self_only'
            AND rg.principal_id IN (SELECT ou_id FROM reader_scope))
       OR  (rg.principal_applies_to = 'self_and_descendants'
            AND rg.principal_id IN (SELECT ou_id FROM reader_grant_keys))
  UNION
    -- Arm 3: org_unit grant that (via org_unit_closure) covers the
    -- document's owner. resource_applies_to=self_only requires the
    -- grant's OU to BE the owner (depth 0); self_and_descendants
    -- covers the whole subtree.
    SELECT c.doc_id FROM cand c
    JOIN   org_unit_closure rc
        ON rc.company_id    = p_company_id
       AND rc.descendant_id = c.owner
    JOIN   resource_grants rg
        ON rg.company_id    = p_company_id
       AND rg.resource_type = 'org_unit'
       AND rg.resource_id   = rc.ancestor_id
       AND rg.permission IN ('read','write','admin')
       AND (rg.expires_at IS NULL OR rg.expires_at > now())
       AND (rg.resource_applies_to = 'self_and_descendants'
            OR (rg.resource_applies_to = 'self_only' AND rc.depth = 0))
    WHERE  (rg.principal_applies_to = 'self_only'
            AND rg.principal_id IN (SELECT ou_id FROM reader_scope))
       OR  (rg.principal_applies_to = 'self_and_descendants'
            AND rg.principal_id IN (SELECT ou_id FROM reader_grant_keys));
$function$;

COMMENT ON FUNCTION wikore_visible_doc_ids(uuid, uuid[], uuid[]) IS
'Shared visibility arms used by G1 (EvidenceGate) and G2 '
'(RelationshipEvidenceGate). Returns the subset of '
'p_candidate_doc_ids that p_reader_scope may access under the '
'three ACL arms (ownership, direct-document grant, org-unit grant '
'with closure). Lifecycle and sensitivity filtering are the '
'caller''s responsibility. See V038 header for the full contract.';
