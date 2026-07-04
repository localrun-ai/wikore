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
    decision_count        INTEGER     NOT NULL DEFAULT 0
                              CONSTRAINT privileged_access_sessions_decision_count_chk
                              CHECK (decision_count >= 0),
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

CREATE OR REPLACE FUNCTION privileged_access_sessions_initial_state_fn()
RETURNS TRIGGER LANGUAGE plpgsql AS $$
BEGIN
    IF NEW.status <> 'pending' OR NEW.activated_at IS NOT NULL
       OR NEW.revoked_at IS NOT NULL OR NEW.revoked_by IS NOT NULL
       OR NEW.revocation_reason IS NOT NULL OR NEW.decision_count <> 0 THEN
        RAISE EXCEPTION
            'new privileged_access sessions must start pending with no transition metadata'
            USING ERRCODE = 'check_violation',
                  CONSTRAINT = 'privileged_access_sessions_initial_state';
    END IF;
    RETURN NEW;
END;
$$;

CREATE TRIGGER privileged_access_sessions_initial_state
    BEFORE INSERT ON privileged_access_sessions
    FOR EACH ROW EXECUTE FUNCTION privileged_access_sessions_initial_state_fn();

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
    FOREIGN KEY (company_id, session_id)
        REFERENCES privileged_access_sessions(company_id, id) ON DELETE CASCADE,
    FOREIGN KEY (company_id, approver_user_id)
        REFERENCES users(company_id, id) ON DELETE CASCADE
);

CREATE INDEX privileged_access_approvals_session_idx
    ON privileged_access_approvals (session_id);

-- Append-only enforcement: approval rows cannot be UPDATE'd or DELETE'd
-- (matches wiki_page_version_tombstones pattern from V025).
CREATE OR REPLACE FUNCTION privileged_access_approvals_append_only()
RETURNS TRIGGER LANGUAGE plpgsql AS $$
BEGIN
    IF TG_OP = 'DELETE'
       AND (NOT EXISTS (SELECT 1 FROM companies WHERE id = OLD.company_id)
            OR NOT EXISTS (SELECT 1 FROM privileged_access_sessions
                           WHERE id = OLD.session_id)) THEN
        -- Parent deletion cascade. The session's BEFORE DELETE history
        -- snapshot has already preserved this decision.
        RETURN OLD;
    END IF;
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

CREATE TRIGGER privileged_access_approvals_no_truncate
    BEFORE TRUNCATE ON privileged_access_approvals
    FOR EACH STATEMENT EXECUTE FUNCTION privileged_access_approvals_append_only();

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
    -- Serialize approval insertion with every session/scope mutation. Without
    -- this lock, an actor or policy UPDATE can observe no uncommitted approval
    -- while this trigger observes the old session, allowing both to commit.
    SELECT * INTO session_row FROM privileged_access_sessions
        WHERE company_id = NEW.company_id AND id = NEW.session_id
        FOR UPDATE;
    IF NOT FOUND THEN
        -- Let the composite FK produce the authoritative tenant/parent error.
        RETURN NEW;
    END IF;
    IF session_row.status <> 'pending' THEN
        RAISE EXCEPTION
            'approval decisions can only be recorded while session % is pending',
            NEW.session_id
            USING ERRCODE = 'check_violation',
                  CONSTRAINT = 'privileged_access_approvals_session_pending';
    END IF;
    IF NOT EXISTS (
        SELECT 1 FROM privileged_access_scopes
        WHERE company_id = NEW.company_id AND session_id = NEW.session_id
    ) THEN
        RAISE EXCEPTION
            'privileged_access session % must have a scope before approval',
            NEW.session_id
            USING ERRCODE = 'check_violation',
                  CONSTRAINT = 'privileged_access_approvals_scope_required';
    END IF;
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

CREATE OR REPLACE FUNCTION privileged_access_approvals_increment_count_fn()
RETURNS TRIGGER LANGUAGE plpgsql AS $$
BEGIN
    UPDATE privileged_access_sessions
       SET decision_count = decision_count + 1
     WHERE company_id = NEW.company_id AND id = NEW.session_id;
    RETURN NEW;
END;
$$;

CREATE TRIGGER privileged_access_approvals_increment_count
    AFTER INSERT ON privileged_access_approvals
    FOR EACH ROW EXECUTE FUNCTION privileged_access_approvals_increment_count_fn();

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
        REFERENCES privileged_access_sessions(company_id, id) ON DELETE CASCADE,
    FOREIGN KEY (company_id, org_unit_id)
        REFERENCES org_units(company_id, id) ON DELETE CASCADE
);

CREATE INDEX privileged_access_scopes_session_idx
    ON privileged_access_scopes (session_id);

-- ---------------------------------------------------------------------------
-- Approval binding and scope immutability.
--
-- Rationale: the separation-of-duties trigger and the require_dual_approval
-- rule compare NEW.approver_user_id against session_row.subject_user_id and
-- session_row.requested_by. If those columns can be UPDATEd after approvals
-- exist, an attacker with UPDATE rights on the session table can swap the
-- subject or requester to a colleague and retroactively defeat both checks.
-- Once ANY decision references the session, reject changes to every field
-- that defines what was approved. Status and transition metadata remain
-- writable, but are validated separately by the state-machine trigger below.
-- ---------------------------------------------------------------------------

CREATE OR REPLACE FUNCTION privileged_access_sessions_lock_request_after_decision()
RETURNS TRIGGER LANGUAGE plpgsql AS $$
BEGIN
    IF NEW.id IS DISTINCT FROM OLD.id OR NEW.company_id IS DISTINCT FROM OLD.company_id THEN
        RAISE EXCEPTION 'privileged_access session identity is immutable'
            USING ERRCODE = 'insufficient_privilege',
                  CONSTRAINT = 'privileged_access_sessions_identity_immutable';
    END IF;
    IF NEW.decision_count IS DISTINCT FROM OLD.decision_count
       AND (pg_trigger_depth() < 2 OR NEW.decision_count <> OLD.decision_count + 1) THEN
        RAISE EXCEPTION 'privileged_access decision_count is trigger-managed'
            USING ERRCODE = 'insufficient_privilege',
                  CONSTRAINT = 'privileged_access_sessions_decision_count_managed';
    END IF;
    IF NEW.subject_user_id IS NOT DISTINCT FROM OLD.subject_user_id
       AND NEW.requested_by IS NOT DISTINCT FROM OLD.requested_by
       AND NEW.purpose IS NOT DISTINCT FROM OLD.purpose
       AND NEW.access_kind IS NOT DISTINCT FROM OLD.access_kind
       AND NEW.starts_at IS NOT DISTINCT FROM OLD.starts_at
       AND NEW.expires_at IS NOT DISTINCT FROM OLD.expires_at
       AND NEW.allow_llm IS NOT DISTINCT FROM OLD.allow_llm
       AND NEW.require_dual_approval IS NOT DISTINCT FROM OLD.require_dual_approval THEN
        RETURN NEW;
    END IF;
    -- OLD is the latest row version after EvalPlanQual. Unlike a subquery
    -- under READ COMMITTED, this counter therefore observes an approval that
    -- committed while this UPDATE was blocked on the session row lock.
    IF OLD.status <> 'pending' OR OLD.decision_count > 0 THEN
        RAISE EXCEPTION
            'privileged_access request fields are immutable once decisions exist for session %',
            OLD.id
            USING ERRCODE = 'insufficient_privilege',
                  CONSTRAINT = 'privileged_access_sessions_request_immutable_after_decision';
    END IF;
    RETURN NEW;
END;
$$;

CREATE TRIGGER privileged_access_sessions_lock_request
    BEFORE UPDATE ON privileged_access_sessions
    FOR EACH ROW EXECUTE FUNCTION privileged_access_sessions_lock_request_after_decision();

CREATE OR REPLACE FUNCTION privileged_access_scopes_mutable_only_before_decision()
RETURNS TRIGGER LANGUAGE plpgsql AS $$
DECLARE
    target_session_id UUID := COALESCE(NEW.session_id, OLD.session_id);
    target_company_id UUID := COALESCE(NEW.company_id, OLD.company_id);
    session_status TEXT;
BEGIN
    IF TG_OP = 'DELETE' THEN
        IF NOT EXISTS (SELECT 1 FROM companies WHERE id = OLD.company_id) THEN
            RETURN OLD; -- tenant cascade
        END IF;
        IF NOT EXISTS (SELECT 1 FROM privileged_access_sessions
                       WHERE id = OLD.session_id) THEN
            RETURN OLD; -- session/company cascade
        END IF;
        IF NOT EXISTS (SELECT 1 FROM org_units
                       WHERE company_id = OLD.company_id AND id = OLD.org_unit_id) THEN
            RETURN OLD; -- org-unit cascade; history retains the approved scope
        END IF;
    END IF;

    SELECT status INTO session_status
      FROM privileged_access_sessions
     WHERE company_id = target_company_id AND id = target_session_id
     FOR UPDATE;
    IF NOT FOUND THEN
        -- Let the composite FK produce the authoritative tenant/parent error.
        RETURN COALESCE(NEW, OLD);
    END IF;

    IF session_status <> 'pending'
       OR EXISTS (SELECT 1 FROM privileged_access_approvals
                  WHERE session_id = target_session_id) THEN
        RAISE EXCEPTION
            'privileged_access scopes are immutable once a decision exists for session %',
            target_session_id
            USING ERRCODE = 'insufficient_privilege',
                  CONSTRAINT = 'privileged_access_scopes_immutable_after_decision';
    END IF;
    RETURN COALESCE(NEW, OLD);
END;
$$;

CREATE TRIGGER privileged_access_scopes_mutability
    BEFORE INSERT OR UPDATE OR DELETE ON privileged_access_scopes
    FOR EACH ROW EXECUTE FUNCTION privileged_access_scopes_mutable_only_before_decision();

-- ---------------------------------------------------------------------------
-- Database-enforced workflow state machine.
-- ---------------------------------------------------------------------------

CREATE OR REPLACE FUNCTION privileged_access_sessions_validate_transition()
RETURNS TRIGGER LANGUAGE plpgsql AS $$
DECLARE
    approved_count INTEGER;
    rejected_count INTEGER;
    required_count INTEGER;
BEGIN
    IF NEW.status IS NOT DISTINCT FROM OLD.status THEN
        IF NEW.activated_at IS DISTINCT FROM OLD.activated_at
           OR NEW.revoked_at IS DISTINCT FROM OLD.revoked_at
           OR NEW.revoked_by IS DISTINCT FROM OLD.revoked_by
           OR NEW.revocation_reason IS DISTINCT FROM OLD.revocation_reason THEN
            RAISE EXCEPTION 'transition metadata may only change with session status'
                USING ERRCODE = 'check_violation',
                      CONSTRAINT = 'privileged_access_sessions_transition_metadata';
        END IF;
        RETURN NEW;
    END IF;

    IF NOT (
        (OLD.status = 'pending'  AND NEW.status IN ('approved','rejected','expired','revoked')) OR
        (OLD.status = 'approved' AND NEW.status IN ('active','expired','revoked')) OR
        (OLD.status = 'active'   AND NEW.status IN ('expired','revoked'))
    ) THEN
        RAISE EXCEPTION 'illegal privileged_access transition % -> %', OLD.status, NEW.status
            USING ERRCODE = 'check_violation',
                  CONSTRAINT = 'privileged_access_sessions_legal_transition';
    END IF;

    SELECT count(*) FILTER (WHERE decision = 'approved'),
           count(*) FILTER (WHERE decision = 'rejected')
      INTO approved_count, rejected_count
      FROM privileged_access_approvals
     WHERE session_id = OLD.id;
    required_count := CASE WHEN NEW.require_dual_approval THEN 2 ELSE 1 END;

    IF NEW.status IN ('approved', 'active') THEN
        IF rejected_count > 0 OR approved_count < required_count THEN
            RAISE EXCEPTION
                'session % needs % approvals and no rejection (approved %, rejected %)',
                OLD.id, required_count, approved_count, rejected_count
                USING ERRCODE = 'insufficient_privilege',
                      CONSTRAINT = 'privileged_access_sessions_approval_requirement';
        END IF;
        IF NOT EXISTS (SELECT 1 FROM privileged_access_scopes WHERE session_id = OLD.id) THEN
            RAISE EXCEPTION 'session % has no approved scope', OLD.id
                USING ERRCODE = 'insufficient_privilege',
                      CONSTRAINT = 'privileged_access_sessions_scope_required';
        END IF;
    END IF;

    IF NEW.status = 'rejected' AND rejected_count = 0 THEN
        RAISE EXCEPTION 'session % cannot be rejected without a rejection decision', OLD.id
            USING ERRCODE = 'check_violation',
                  CONSTRAINT = 'privileged_access_sessions_rejection_requirement';
    END IF;

    IF NEW.status = 'active' THEN
        IF clock_timestamp() < NEW.starts_at OR clock_timestamp() >= NEW.expires_at THEN
            RAISE EXCEPTION 'session % is outside its approved time window', OLD.id
                USING ERRCODE = 'insufficient_privilege',
                      CONSTRAINT = 'privileged_access_sessions_active_time_window';
        END IF;
        NEW.activated_at := clock_timestamp();
    ELSIF NEW.activated_at IS DISTINCT FROM OLD.activated_at THEN
        RAISE EXCEPTION 'activated_at is set only by transition to active'
            USING ERRCODE = 'check_violation',
                  CONSTRAINT = 'privileged_access_sessions_activated_at';
    END IF;

    IF NEW.status = 'expired' AND clock_timestamp() < NEW.expires_at THEN
        RAISE EXCEPTION 'session % cannot expire before expires_at', OLD.id
            USING ERRCODE = 'check_violation',
                  CONSTRAINT = 'privileged_access_sessions_expiry_time';
    END IF;

    IF NEW.status = 'revoked' THEN
        IF NEW.revoked_at IS NULL OR NEW.revoked_by IS NULL
           OR length(btrim(COALESCE(NEW.revocation_reason, ''))) < 5 THEN
            RAISE EXCEPTION 'revocation requires actor, timestamp, and reason'
                USING ERRCODE = 'check_violation',
                      CONSTRAINT = 'privileged_access_sessions_revocation_metadata';
        END IF;
    ELSIF NEW.revoked_at IS NOT NULL OR NEW.revoked_by IS NOT NULL
          OR NEW.revocation_reason IS NOT NULL THEN
        RAISE EXCEPTION 'revocation metadata is only valid for revoked sessions'
            USING ERRCODE = 'check_violation',
                  CONSTRAINT = 'privileged_access_sessions_revocation_metadata';
    END IF;
    RETURN NEW;
END;
$$;

CREATE TRIGGER privileged_access_sessions_state_machine
    BEFORE UPDATE ON privileged_access_sessions
    FOR EACH ROW EXECUTE FUNCTION privileged_access_sessions_validate_transition();

-- ---------------------------------------------------------------------------
-- Append-only transition history. There is deliberately no FK to the live
-- session/company: audit evidence must retain its identifiers after an
-- explicitly-authorized tenant-erasure operation.
-- ---------------------------------------------------------------------------

CREATE TABLE privileged_access_session_history (
    id             BIGSERIAL PRIMARY KEY,
    live_session_id UUID        NOT NULL,
    company_id     UUID         NOT NULL,
    change_kind    TEXT         NOT NULL CHECK (change_kind IN ('insert','transition','delete')),
    old_status     TEXT,
    new_status     TEXT         NOT NULL,
    request_snapshot JSONB      NOT NULL,
    scope_snapshot JSONB        NOT NULL,
    approval_snapshot JSONB     NOT NULL,
    database_actor TEXT         NOT NULL DEFAULT session_user,
    changed_at     TIMESTAMPTZ  NOT NULL DEFAULT clock_timestamp()
);

CREATE INDEX privileged_access_session_history_lookup_idx
    ON privileged_access_session_history (company_id, live_session_id, changed_at DESC);

CREATE OR REPLACE FUNCTION privileged_access_sessions_write_history()
RETURNS TRIGGER LANGUAGE plpgsql AS $$
BEGIN
    IF TG_OP = 'UPDATE' AND NEW.status IS NOT DISTINCT FROM OLD.status THEN
        RETURN NEW;
    END IF;
    INSERT INTO privileged_access_session_history
        (live_session_id, company_id, change_kind, old_status, new_status,
         request_snapshot, scope_snapshot, approval_snapshot)
    VALUES
        (NEW.id, NEW.company_id,
         CASE WHEN TG_OP = 'INSERT' THEN 'insert' ELSE 'transition' END,
         CASE WHEN TG_OP = 'INSERT' THEN NULL ELSE OLD.status END,
         NEW.status,
         jsonb_build_object(
             'subject_user_id', NEW.subject_user_id,
             'requested_by', NEW.requested_by,
             'purpose', NEW.purpose,
             'access_kind', NEW.access_kind,
             'starts_at', NEW.starts_at,
             'expires_at', NEW.expires_at,
             'allow_llm', NEW.allow_llm,
             'require_dual_approval', NEW.require_dual_approval,
             'decision_count', NEW.decision_count,
             'activated_at', NEW.activated_at,
             'revoked_at', NEW.revoked_at,
             'revoked_by', NEW.revoked_by,
             'revocation_reason', NEW.revocation_reason),
         COALESCE((
             SELECT jsonb_agg(to_jsonb(s) - 'company_id' - 'session_id'
                              ORDER BY s.org_unit_id)
              FROM privileged_access_scopes s
              WHERE s.session_id = NEW.id
         ), '[]'::jsonb),
         COALESCE((
             SELECT jsonb_agg(to_jsonb(a) - 'company_id' - 'session_id'
                              ORDER BY a.decided_at, a.approver_user_id)
               FROM privileged_access_approvals a
              WHERE a.session_id = NEW.id
         ), '[]'::jsonb));
    RETURN NEW;
END;
$$;

CREATE TRIGGER privileged_access_sessions_history
    AFTER INSERT OR UPDATE ON privileged_access_sessions
    FOR EACH ROW EXECUTE FUNCTION privileged_access_sessions_write_history();

-- Capture the final live request, scopes, and decisions before child CASCADEs
-- run. This is the durable audit record that permits operational rows to be
-- removed during tenant offboarding without losing historical evidence.
CREATE OR REPLACE FUNCTION privileged_access_sessions_write_delete_history()
RETURNS TRIGGER LANGUAGE plpgsql AS $$
BEGIN
    INSERT INTO privileged_access_session_history
        (live_session_id, company_id, change_kind, old_status, new_status,
         request_snapshot, scope_snapshot, approval_snapshot)
    VALUES
        (OLD.id, OLD.company_id, 'delete', OLD.status, OLD.status,
         jsonb_build_object(
             'subject_user_id', OLD.subject_user_id,
             'requested_by', OLD.requested_by,
             'purpose', OLD.purpose,
             'access_kind', OLD.access_kind,
             'starts_at', OLD.starts_at,
             'expires_at', OLD.expires_at,
             'allow_llm', OLD.allow_llm,
             'require_dual_approval', OLD.require_dual_approval,
             'decision_count', OLD.decision_count,
             'activated_at', OLD.activated_at,
             'revoked_at', OLD.revoked_at,
             'revoked_by', OLD.revoked_by,
             'revocation_reason', OLD.revocation_reason),
         COALESCE((
             SELECT jsonb_agg(to_jsonb(s) - 'company_id' - 'session_id'
                              ORDER BY s.org_unit_id)
               FROM privileged_access_scopes s
              WHERE s.session_id = OLD.id
         ), '[]'::jsonb),
         COALESCE((
             SELECT jsonb_agg(to_jsonb(a) - 'company_id' - 'session_id'
                              ORDER BY a.decided_at, a.approver_user_id)
               FROM privileged_access_approvals a
              WHERE a.session_id = OLD.id
         ), '[]'::jsonb));
    RETURN OLD;
END;
$$;

CREATE TRIGGER privileged_access_sessions_delete_history
    BEFORE DELETE ON privileged_access_sessions
    FOR EACH ROW EXECUTE FUNCTION privileged_access_sessions_write_delete_history();

CREATE OR REPLACE FUNCTION privileged_access_history_append_only()
RETURNS TRIGGER LANGUAGE plpgsql AS $$
BEGIN
    RAISE EXCEPTION 'privileged_access_session_history is append-only; % rejected', TG_OP
        USING ERRCODE = 'insufficient_privilege',
              CONSTRAINT = 'privileged_access_session_history_append_only';
END;
$$;

CREATE TRIGGER privileged_access_session_history_no_update
    BEFORE UPDATE ON privileged_access_session_history
    FOR EACH ROW EXECUTE FUNCTION privileged_access_history_append_only();
CREATE TRIGGER privileged_access_session_history_no_delete
    BEFORE DELETE ON privileged_access_session_history
    FOR EACH ROW EXECUTE FUNCTION privileged_access_history_append_only();
CREATE TRIGGER privileged_access_session_history_no_truncate
    BEFORE TRUNCATE ON privileged_access_session_history
    FOR EACH STATEMENT EXECUTE FUNCTION privileged_access_history_append_only();
