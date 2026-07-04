-- V036: BaryGraph Lite privileged access sessions
--
-- Third of three BaryGraph Lite migrations (see docs/barygraph_features.md).
-- Independent from V034 (edges) and V035 (capabilities): a tenant may use
-- privileged access without any edges configured, and capability grants
-- work without privileged sessions.
--
-- Purpose:
-- Sensitive analysis (relationship discovery over restricted-source, break-
-- glass access, retrieval diagnostics observing rejected candidates,
-- temporary consultant/legal engagements) needs explicit workflow:
-- requested → approved → active → expired/revoked. This is orthogonal to
-- ordinary long-lived capability entitlement — a user may hold
-- 'relationship_search' capability continuously yet still need a scoped,
-- reason-attributed session to access unusually sensitive material or
-- outside their normal scope.
--
-- Fail-closed contract (see docs/barygraph_features.md §"Fail-closed database flow"):
--   1. Lock the session under a consistent snapshot.
--   2. Verify tenant, subject, status, start, expiry, approvals, capability.
--   3. Resolve approved temporary scopes.
--   4. Run authoritative all-endpoint EvidenceGate.
--   5. Insert audit event with the exact allowed source/edge IDs.
--   6. Commit BEFORE any source text is released.
-- If any step fails, no sensitive evidence is returned. Logging failure IS
-- an authorization failure.
--
-- Session duration is enforced at the application layer, not by a CHECK
-- constraint. Product policy sets maxima (e.g. 30 min break-glass, 24 h
-- legal review, 72 h consultant engagement); baking those into the schema
-- forces a migration to change them.

-- ---------------------------------------------------------------------------
-- privileged_access_sessions
-- ---------------------------------------------------------------------------

CREATE TABLE privileged_access_sessions (
    id                    UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    company_id            UUID        NOT NULL
                              REFERENCES companies(id) ON DELETE CASCADE,
    subject_user_id       UUID        NOT NULL,
    requested_by          UUID        NOT NULL,
    purpose               TEXT        NOT NULL
                              CHECK (length(btrim(purpose)) >= 10),
    access_kind           TEXT        NOT NULL CHECK (access_kind IN (
                              'temporary_engagement',
                              'sensitive_analysis',
                              'retrieval_diagnostics',
                              'break_glass'
                          )),
    status                TEXT        NOT NULL DEFAULT 'pending'
                              CHECK (status IN (
                                  'pending', 'approved', 'active',
                                  'expired', 'revoked', 'rejected'
                              )),
    starts_at             TIMESTAMPTZ NOT NULL,
    expires_at            TIMESTAMPTZ NOT NULL,
    allow_llm             BOOLEAN     NOT NULL DEFAULT false,
    require_dual_approval BOOLEAN     NOT NULL DEFAULT false,
    activated_at          TIMESTAMPTZ,
    revoked_at            TIMESTAMPTZ,
    revoked_by            UUID,
    revocation_reason     TEXT,
    created_at            TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at            TIMESTAMPTZ NOT NULL DEFAULT now(),
    CHECK (expires_at > starts_at),
    -- Session duration maxima are product policy, not schema invariants.
    -- Enforce in application (allows per-tenant / per-access-kind maxima
    -- without a migration).
    CHECK ((revoked_at IS NULL) = (revoked_by IS NULL)),
    FOREIGN KEY (company_id, subject_user_id)
        REFERENCES users(company_id, id),
    FOREIGN KEY (company_id, requested_by)
        REFERENCES users(company_id, id),
    FOREIGN KEY (company_id, revoked_by)
        REFERENCES users(company_id, id),
    -- Composite-FK target: child tables (approvals, scopes) bind
    -- (company_id, session_id) to (company_id, id) so cross-tenant rows
    -- cannot attach to another company's session.
    UNIQUE (company_id, id)
);

CREATE INDEX privileged_access_sessions_subject_idx
    ON privileged_access_sessions (company_id, subject_user_id, status);
CREATE INDEX privileged_access_sessions_status_idx
    ON privileged_access_sessions (company_id, status)
    WHERE status IN ('active', 'approved');
CREATE INDEX privileged_access_sessions_expires_idx
    ON privileged_access_sessions (expires_at)
    WHERE status IN ('active', 'approved');

CREATE TRIGGER privileged_access_sessions_updated_at
    BEFORE UPDATE ON privileged_access_sessions
    FOR EACH ROW EXECUTE FUNCTION set_updated_at();

COMMENT ON TABLE privileged_access_sessions IS
    'Explicit workflow-gated sessions for sensitive relationship analysis, '
    'break-glass, diagnostics, and temporary consultant/legal engagements. '
    'See docs/barygraph_features.md §"Privileged and high-sensitivity access schema".';

-- ---------------------------------------------------------------------------
-- privileged_access_approvals
-- ---------------------------------------------------------------------------
-- Append-only: an approval decision, once recorded, is not modified. A
-- changed decision is a new workflow (new session), not an UPDATE that
-- rewrites history. Enforced by the trigger below.

CREATE TABLE privileged_access_approvals (
    session_id       UUID        NOT NULL,
    company_id       UUID        NOT NULL,
    approver_user_id UUID        NOT NULL,
    decision         TEXT        NOT NULL CHECK (decision IN ('approved', 'rejected')),
    reason           TEXT        NOT NULL CHECK (length(btrim(reason)) >= 5),
    decided_at       TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (session_id, approver_user_id),
    -- Composite FK ties (company_id, session_id) to the session's own
    -- (company_id, id). Combined with the composite approver FK below,
    -- approver, approval row, and session are transitively forced into
    -- the same tenant — nothing can attach a cross-tenant approval.
    -- ON DELETE RESTRICT because approvals are audit records that must
    -- outlive stray parent deletes (which are also blocked by other FKs).
    FOREIGN KEY (company_id, session_id)
        REFERENCES privileged_access_sessions(company_id, id) ON DELETE RESTRICT,
    FOREIGN KEY (company_id, approver_user_id)
        REFERENCES users(company_id, id)
);

CREATE INDEX privileged_access_approvals_session_idx
    ON privileged_access_approvals (session_id);

-- Append-only enforcement: approval rows cannot be UPDATE'd or DELETE'd
-- (matches wiki_page_version_tombstones pattern from V025).
CREATE OR REPLACE FUNCTION privileged_access_approvals_append_only()
RETURNS TRIGGER LANGUAGE plpgsql AS $$
BEGIN
    RAISE EXCEPTION 'privileged_access_approvals is append-only; % rejected', TG_OP
        USING ERRCODE = 'insufficient_privilege',
              CONSTRAINT = 'privileged_access_approvals_append_only';
END;
$$;

CREATE TRIGGER privileged_access_approvals_no_update
    BEFORE UPDATE ON privileged_access_approvals
    FOR EACH ROW EXECUTE FUNCTION privileged_access_approvals_append_only();

CREATE TRIGGER privileged_access_approvals_no_delete
    BEFORE DELETE ON privileged_access_approvals
    FOR EACH ROW EXECUTE FUNCTION privileged_access_approvals_append_only();

-- Separation-of-duties: subject/requester cannot approve their own session.
-- (Company-scoped by the composite session/approver FKs above, so cross-
-- tenant impersonation is already impossible.)
--
-- Note on dual-approval sessions: this BEFORE INSERT trigger cannot verify
-- that TWO distinct approvals exist — activation logic must count
-- decision='approved' rows and check >= 2 distinct approvers for a
-- require_dual_approval session. This trigger enforces the local
-- invariant: the requester cannot record an 'approved' or 'rejected'
-- decision on a dual-approval session AT ALL — not as the sole approver
-- and not as one of the two. That is stricter than the sole-approver
-- literal reading but is the safer policy: it makes dual-approval a real
-- second-pair-of-eyes control, not a rubber stamp co-signed by the
-- requester after a colleague clicks approve.
CREATE OR REPLACE FUNCTION privileged_access_approvals_separation_of_duties()
RETURNS TRIGGER LANGUAGE plpgsql AS $$
DECLARE
    session_row privileged_access_sessions%ROWTYPE;
BEGIN
    SELECT * INTO session_row FROM privileged_access_sessions
        WHERE id = NEW.session_id;
    IF NEW.approver_user_id = session_row.subject_user_id THEN
        RAISE EXCEPTION
            'privileged_access approver cannot be the subject of the session'
            USING ERRCODE = 'insufficient_privilege',
                  CONSTRAINT = 'privileged_access_approvals_no_self_approval';
    END IF;
    IF NEW.approver_user_id = session_row.requested_by
       AND session_row.require_dual_approval THEN
        RAISE EXCEPTION
            'privileged_access requester cannot approve a dual-approval session'
            USING ERRCODE = 'insufficient_privilege',
                  CONSTRAINT = 'privileged_access_approvals_no_requester_approval';
    END IF;
    RETURN NEW;
END;
$$;

CREATE TRIGGER privileged_access_approvals_separation_of_duties_trg
    BEFORE INSERT ON privileged_access_approvals
    FOR EACH ROW EXECUTE FUNCTION privileged_access_approvals_separation_of_duties();

-- ---------------------------------------------------------------------------
-- privileged_access_scopes
-- ---------------------------------------------------------------------------

CREATE TABLE privileged_access_scopes (
    session_id            UUID NOT NULL,
    company_id            UUID NOT NULL,
    org_unit_id           UUID NOT NULL,
    applies_to            TEXT NOT NULL CHECK (applies_to IN (
                              'self_only', 'self_and_descendants'
                          )),
    maximum_sensitivity   TEXT NOT NULL CHECK (maximum_sensitivity IN (
                              'public', 'internal', 'confidential', 'restricted'
                          )),
    allow_relationships   BOOLEAN NOT NULL DEFAULT true,
    allow_impact_analysis BOOLEAN NOT NULL DEFAULT false,
    allow_diagnostics     BOOLEAN NOT NULL DEFAULT false,
    PRIMARY KEY (session_id, org_unit_id),
    -- Composite FK ties (company_id, session_id) to the session's own
    -- (company_id, id) so scopes cannot attach one tenant's org units to
    -- another tenant's session. The org_units composite FK below then
    -- enforces same-company for the org unit too.
    FOREIGN KEY (company_id, session_id)
        REFERENCES privileged_access_sessions(company_id, id) ON DELETE RESTRICT,
    FOREIGN KEY (company_id, org_unit_id)
        REFERENCES org_units(company_id, id) ON DELETE CASCADE
);

CREATE INDEX privileged_access_scopes_session_idx
    ON privileged_access_scopes (session_id);

-- ---------------------------------------------------------------------------
-- Immutable-once-approved guard on privileged_access_sessions.
--
-- Rationale: the separation-of-duties trigger and the require_dual_approval
-- rule compare NEW.approver_user_id against session_row.subject_user_id and
-- session_row.requested_by. If those columns can be UPDATEd after approvals
-- exist, an attacker with UPDATE rights on the session table can swap the
-- subject or requester to a colleague and retroactively defeat both checks.
-- Once ANY approval row references the session, reject changes to those two
-- columns. Everything else (status, activated_at, revoked_at, ...) stays
-- editable so the workflow can progress.
-- ---------------------------------------------------------------------------

CREATE OR REPLACE FUNCTION privileged_access_sessions_lock_actors_after_approval()
RETURNS TRIGGER LANGUAGE plpgsql AS $$
BEGIN
    IF NEW.subject_user_id = OLD.subject_user_id
       AND NEW.requested_by = OLD.requested_by THEN
        RETURN NEW;
    END IF;
    IF EXISTS (SELECT 1 FROM privileged_access_approvals WHERE session_id = OLD.id) THEN
        RAISE EXCEPTION
            'subject_user_id and requested_by are immutable once approvals exist for session %',
            OLD.id
            USING ERRCODE = 'insufficient_privilege',
                  CONSTRAINT = 'privileged_access_sessions_actors_immutable_after_approval';
    END IF;
    RETURN NEW;
END;
$$;

CREATE TRIGGER privileged_access_sessions_lock_actors
    BEFORE UPDATE ON privileged_access_sessions
    FOR EACH ROW EXECUTE FUNCTION privileged_access_sessions_lock_actors_after_approval();
