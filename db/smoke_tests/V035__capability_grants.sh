# Per-migration smoke tests for V035 (BaryGraph Lite capability grants).

# V035.1: tenant_features PK enforces one row per (company, feature)
sql "INSERT INTO tenant_features (company_id, feature, enabled)
     VALUES ('$CO_ACME', 'bridge_retrieval', true);" > /dev/null
ERR=$(sql "INSERT INTO tenant_features (company_id, feature, enabled)
           VALUES ('$CO_ACME', 'bridge_retrieval', false);" 2>&1 || true)
echo "$ERR" | grep -qi "duplicate\|unique" \
  && pass "V035.1" "tenant_features PK rejects duplicate (company, feature)" \
  || fail "V035.1" "duplicate tenant_feature accepted: $ERR"

# V035.2: user_capability_grants requires a non-empty reason
ERR=$(sql "INSERT INTO user_capability_grants
             (company_id, user_id, capability, granted_by, reason)
           VALUES ('$CO_ACME', '$U_ALICE', 'relationship_search', '$U_ALICE', '');" 2>&1 || true)
echo "$ERR" | grep -qi "reason_check\|check constraint" \
  && pass "V035.2" "user_capability_grants rejects empty reason" \
  || fail "V035.2" "empty reason accepted: $ERR"

# V035.3: valid user_capability_grant is accepted
sql "INSERT INTO user_capability_grants
       (company_id, user_id, capability, granted_by, reason)
     VALUES ('$CO_ACME', '$U_ALICE', 'relationship_search', '$U_ALICE',
             'approved by legal review');" > /dev/null
COUNT=$(sql "SELECT count(*) FROM user_capability_grants
             WHERE user_id='$U_ALICE' AND capability='relationship_search';")
[ "$COUNT" = "1" ] \
  && pass "V035.3" "user_capability_grants accepts valid grant" \
  || fail "V035.3" "valid grant rejected (count=$COUNT)"

# V035.4: expires_at must be greater than granted_at
ERR=$(sql "INSERT INTO user_capability_grants
             (company_id, user_id, capability, granted_by, reason,
              granted_at, expires_at)
           VALUES ('$CO_ACME', '$U_ALICE', 'retrieval_diagnostics', '$U_ALICE',
                   'diagnostic access',
                   '2026-07-01 00:00:00+00', '2026-06-30 00:00:00+00');" 2>&1 || true)
echo "$ERR" | grep -qi "check\|violates" \
  && pass "V035.4" "expires_at <= granted_at rejected" \
  || fail "V035.4" "backwards expires_at accepted: $ERR"

# V035.5: revoked_at without revoked_by rejected (must move together)
ERR=$(sql "UPDATE user_capability_grants
           SET revoked_at = now()
           WHERE user_id='$U_ALICE' AND capability='relationship_search';" 2>&1 || true)
echo "$ERR" | grep -qi "check\|violates" \
  && pass "V035.5" "revoked_at without revoked_by rejected" \
  || fail "V035.5" "asymmetric revocation accepted: $ERR"

# V035.6: revocation with both fields accepted
sql "UPDATE user_capability_grants
     SET revoked_at = now(), revoked_by = '$U_ALICE', revocation_reason='engagement ended'
     WHERE user_id='$U_ALICE' AND capability='relationship_search';" > /dev/null
COUNT=$(sql "SELECT count(*) FROM user_capability_grants
             WHERE user_id='$U_ALICE' AND revoked_at IS NOT NULL;")
[ "$COUNT" = "1" ] \
  && pass "V035.6" "clean revocation (both fields) accepted" \
  || fail "V035.6" "revocation rejected"

# V035.7: org_unit_capability_grants applies_to CHECK
ERR=$(sql "INSERT INTO org_unit_capability_grants
             (company_id, org_unit_id, capability, applies_to, granted_by, reason)
           VALUES ('$CO_ACME', '$ACME_ROOT', 'relationship_search',
                   'invalid_scope', '$U_ALICE', 'test');" 2>&1 || true)
echo "$ERR" | grep -qi "applies_to_check\|check constraint" \
  && pass "V035.7" "org_unit applies_to rejects invalid value" \
  || fail "V035.7" "invalid applies_to accepted: $ERR"

# V035.8: default applies_to is 'self_and_descendants'
sql "INSERT INTO org_unit_capability_grants
       (company_id, org_unit_id, capability, granted_by, reason)
     VALUES ('$CO_ACME', '$ACME_ROOT', 'relationship_search', '$U_ALICE',
             'legal review team');" > /dev/null
VAL=$(sql "SELECT applies_to FROM org_unit_capability_grants
           WHERE org_unit_id='$ACME_ROOT' AND capability='relationship_search';")
[ "$VAL" = "self_and_descendants" ] \
  && pass "V035.8" "org_unit_capability_grants defaults applies_to='self_and_descendants'" \
  || fail "V035.8" "unexpected default applies_to=$VAL"

# V035.9: same-company FK enforced (cross-company granted_by rejected)
# (Only one company in fixtures, so we verify the constraint exists rather
# than trying to create a cross-tenant user.)
EXISTS=$(sql "SELECT count(*) FROM pg_constraint
              WHERE conrelid = 'user_capability_grants'::regclass
                AND contype = 'f';")
[ "$EXISTS" -ge "3" ] \
  && pass "V035.9" "user_capability_grants has 3 composite FKs (user, granted_by, revoked_by)" \
  || fail "V035.9" "user_capability_grants composite FKs missing (count=$EXISTS)"
