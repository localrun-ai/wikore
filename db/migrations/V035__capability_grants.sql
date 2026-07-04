-- V035: BaryGraph Lite capability grants (feature entitlement layer)
--
-- Second of three BaryGraph Lite migrations (see docs/barygraph_features.md).
-- V035 stands alone from V034 (edges) and V036 (privileged access): a tenant
-- may enable capabilities without ever using knowledge_edges, and privileged
-- access sessions can be gated on capabilities without requiring edges.
--
-- Design: relationship-analysis features (bridge search, impact analysis,
-- retrieval diagnostics, break-glass) require an *entitlement* that is
-- separate from the *data* authorization enforced by EvidenceGate. Both must
-- pass. Neither implies the other.
--
--   tenant_features:      per-company feature flags (admin-managed).
--   user_capability_grants:      per-user capability with reason + expiry.
--   group_capability_grants:     per-group (SSO/local) capability.
--   org_unit_capability_grants:  per-org-unit with self_only / self_and_descendants
--                                semantics (resolved via org_unit_closure at
--                                query time, same pattern as memberships).
--
-- The existing resource_grants model is NOT sufficient for capability
-- entitlement. resource_grants gates access to governed resources (documents,
-- org units) and only permits principals of type 'org_unit'. Feature
-- entitlement is a separate concern (may the user perform this analysis?)
-- and needs a distinct schema.
--
-- Every grant carries reason + granted_by for audit. expires_at + revoked_at
-- support time-limited entitlement (consultant engagements, temporary access).

-- ---------------------------------------------------------------------------
-- tenant_features
-- ---------------------------------------------------------------------------

CREATE TABLE tenant_features (
    company_id   UUID        NOT NULL REFERENCES companies(id) ON DELETE CASCADE,
    feature      TEXT        NOT NULL,
    enabled      BOOLEAN     NOT NULL DEFAULT false,
    enabled_by   UUID,
    enabled_at   TIMESTAMPTZ,
    updated_at   TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (company_id, feature),
    -- Composite same-company FK for the actor. NO ACTION (not SET NULL)
    -- for consistency with V034's actor FKs and because PG17 does not
    -- support per-column SET NULL, which is what a composite FK with a
    -- nullable component would need to avoid nulling the PK column.
    CONSTRAINT tenant_features_enabled_by_same_company_fk
        FOREIGN KEY (company_id, enabled_by)
        REFERENCES users(company_id, id) MATCH SIMPLE
        ON DELETE NO ACTION
);

COMMENT ON TABLE tenant_features IS
    'Per-company feature flags. Admin-managed. See docs/barygraph_features.md.';

CREATE TRIGGER tenant_features_updated_at
    BEFORE UPDATE ON tenant_features
    FOR EACH ROW EXECUTE FUNCTION set_updated_at();

-- ---------------------------------------------------------------------------
-- user_capability_grants
--
-- Append-mostly: rows are INSERTed on grant, UPDATEd only to fill the
-- (revoked_at, revoked_by, revocation_reason) triple, and never PK-collide
-- across re-grants. Surrogate PK + partial unique index on active rows
-- gives us: (1) one live grant per (company, user, capability) enforced by
-- the DB, (2) full historical grant/revoke chain preserved, (3) re-grant is
-- a plain INSERT after the prior row is revoked, (4) the check-side query
-- reads the partial index directly (it filters to revoked_at IS NULL anyway).
-- ---------------------------------------------------------------------------

CREATE TABLE user_capability_grants (
    id           UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    company_id   UUID        NOT NULL,
    user_id      UUID        NOT NULL,
    capability   TEXT        NOT NULL,
    granted_by   UUID        NOT NULL,
    reason       TEXT        NOT NULL CHECK (length(btrim(reason)) >= 3),
    granted_at   TIMESTAMPTZ NOT NULL DEFAULT now(),
    expires_at   TIMESTAMPTZ,
    revoked_at   TIMESTAMPTZ,
    revoked_by   UUID,
    revocation_reason TEXT,
    updated_at   TIMESTAMPTZ NOT NULL DEFAULT now(),
    -- Composite FKs enforce same-company subject/actor.
    FOREIGN KEY (company_id, user_id)
        REFERENCES users(company_id, id) ON DELETE CASCADE,
    FOREIGN KEY (company_id, granted_by)
        REFERENCES users(company_id, id),
    FOREIGN KEY (company_id, revoked_by)
        REFERENCES users(company_id, id),
    -- expires_at must be after granted_at.
    CHECK (expires_at IS NULL OR expires_at > granted_at),
    -- revoked_at and revoked_by move together.
    CHECK ((revoked_at IS NULL) = (revoked_by IS NULL))
);

-- One active (unrevoked) grant per (company, user, capability). Re-grant
-- after revoke is a plain INSERT. Prior audit chain (granted_by, reason,
-- revoked_by, revoked_at, revocation_reason) stays queryable.
CREATE UNIQUE INDEX user_capability_grants_active_uidx
    ON user_capability_grants (company_id, user_id, capability)
    WHERE revoked_at IS NULL;

CREATE TRIGGER user_capability_grants_updated_at
    BEFORE UPDATE ON user_capability_grants
    FOR EACH ROW EXECUTE FUNCTION set_updated_at();

-- ---------------------------------------------------------------------------
-- group_capability_grants — same append-mostly / active-uidx pattern as
-- user_capability_grants (see rationale there).
-- ---------------------------------------------------------------------------

CREATE TABLE group_capability_grants (
    id           UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    company_id   UUID        NOT NULL,
    group_id     UUID        NOT NULL,
    capability   TEXT        NOT NULL,
    granted_by   UUID        NOT NULL,
    reason       TEXT        NOT NULL CHECK (length(btrim(reason)) >= 3),
    granted_at   TIMESTAMPTZ NOT NULL DEFAULT now(),
    expires_at   TIMESTAMPTZ,
    revoked_at   TIMESTAMPTZ,
    revoked_by   UUID,
    revocation_reason TEXT,
    updated_at   TIMESTAMPTZ NOT NULL DEFAULT now(),
    FOREIGN KEY (company_id, group_id)
        REFERENCES groups(company_id, id) ON DELETE CASCADE,
    FOREIGN KEY (company_id, granted_by)
        REFERENCES users(company_id, id),
    FOREIGN KEY (company_id, revoked_by)
        REFERENCES users(company_id, id),
    CHECK (expires_at IS NULL OR expires_at > granted_at),
    CHECK ((revoked_at IS NULL) = (revoked_by IS NULL))
);

CREATE UNIQUE INDEX group_capability_grants_active_uidx
    ON group_capability_grants (company_id, group_id, capability)
    WHERE revoked_at IS NULL;

CREATE TRIGGER group_capability_grants_updated_at
    BEFORE UPDATE ON group_capability_grants
    FOR EACH ROW EXECUTE FUNCTION set_updated_at();

-- ---------------------------------------------------------------------------
-- org_unit_capability_grants — same append-mostly / active-uidx pattern.
-- ---------------------------------------------------------------------------
-- Subtree membership is resolved via org_unit_closure at capability-check
-- time (same pattern as memberships). 'self_and_descendants' grants apply
-- to every descendant of the org_unit; 'self_only' applies to the unit alone.

CREATE TABLE org_unit_capability_grants (
    id           UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    company_id   UUID        NOT NULL,
    org_unit_id  UUID        NOT NULL,
    capability   TEXT        NOT NULL,
    applies_to   TEXT        NOT NULL DEFAULT 'self_and_descendants'
                     CHECK (applies_to IN ('self_only', 'self_and_descendants')),
    granted_by   UUID        NOT NULL,
    reason       TEXT        NOT NULL CHECK (length(btrim(reason)) >= 3),
    granted_at   TIMESTAMPTZ NOT NULL DEFAULT now(),
    expires_at   TIMESTAMPTZ,
    revoked_at   TIMESTAMPTZ,
    revoked_by   UUID,
    revocation_reason TEXT,
    updated_at   TIMESTAMPTZ NOT NULL DEFAULT now(),
    FOREIGN KEY (company_id, org_unit_id)
        REFERENCES org_units(company_id, id) ON DELETE CASCADE,
    FOREIGN KEY (company_id, granted_by)
        REFERENCES users(company_id, id),
    FOREIGN KEY (company_id, revoked_by)
        REFERENCES users(company_id, id),
    CHECK (expires_at IS NULL OR expires_at > granted_at),
    CHECK ((revoked_at IS NULL) = (revoked_by IS NULL))
);

-- Uniqueness on (company, org_unit, capability) is scope-agnostic: two
-- overlapping applies_to values on the same (unit, capability) would be
-- redundant. If callers need both 'self_only' and 'self_and_descendants'
-- simultaneously, the second is a superset and the first should be revoked.
CREATE UNIQUE INDEX org_unit_capability_grants_active_uidx
    ON org_unit_capability_grants (company_id, org_unit_id, capability)
    WHERE revoked_at IS NULL;

CREATE TRIGGER org_unit_capability_grants_updated_at
    BEFORE UPDATE ON org_unit_capability_grants
    FOR EACH ROW EXECUTE FUNCTION set_updated_at();

COMMENT ON TABLE org_unit_capability_grants IS
    'Per-org-unit capability grants with self_only / self_and_descendants '
    'semantics. Resolved via org_unit_closure at check time. See '
    'docs/barygraph_features.md.';
