# Per-migration smoke tests for V036 (BaryGraph Lite privileged access).

SESS1='6a550001-0000-0000-0000-000000000001'
SESS2='6a550002-0000-0000-0000-000000000002'

# V036.1: valid session accepted
sql "INSERT INTO privileged_access_sessions
       (id, company_id, subject_user_id, requested_by, purpose, access_kind,
        starts_at, expires_at)
     VALUES ('$SESS1', '$CO_ACME', '$U_ALICE', '$U_ALICE',
             'Approved legal review for acquisition documents',
             'sensitive_analysis', now(), now() + INTERVAL '4 hours');" > /dev/null
COUNT=$(sql "SELECT count(*) FROM privileged_access_sessions WHERE id='$SESS1';")
[ "$COUNT" = "1" ] \
  && pass "V036.1" "valid privileged_access_session accepted" \
  || fail "V036.1" "valid session rejected (count=$COUNT)"

# V036.2: purpose CHECK (min 10 chars) rejects short purpose
ERR=$(sql "INSERT INTO privileged_access_sessions
             (id, company_id, subject_user_id, requested_by, purpose, access_kind,
              starts_at, expires_at)
           VALUES (gen_random_uuid(), '$CO_ACME', '$U_ALICE', '$U_ALICE',
                   'too short', 'break_glass',
                   now(), now() + INTERVAL '1 hour');" 2>&1 || true)
echo "$ERR" | grep -qi "purpose_check\|check constraint" \
  && pass "V036.2" "short purpose rejected" \
  || fail "V036.2" "short purpose accepted: $ERR"

# V036.3: invalid access_kind rejected
ERR=$(sql "INSERT INTO privileged_access_sessions
             (id, company_id, subject_user_id, requested_by, purpose, access_kind,
              starts_at, expires_at)
           VALUES (gen_random_uuid(), '$CO_ACME', '$U_ALICE', '$U_ALICE',
                   'a proper long purpose string', 'unknown_kind',
                   now(), now() + INTERVAL '1 hour');" 2>&1 || true)
echo "$ERR" | grep -qi "access_kind_check\|check constraint" \
  && pass "V036.3" "invalid access_kind rejected" \
  || fail "V036.3" "invalid access_kind accepted: $ERR"

# V036.4: expires_at <= starts_at rejected
ERR=$(sql "INSERT INTO privileged_access_sessions
             (id, company_id, subject_user_id, requested_by, purpose, access_kind,
              starts_at, expires_at)
           VALUES (gen_random_uuid(), '$CO_ACME', '$U_ALICE', '$U_ALICE',
                   'a proper long purpose string', 'break_glass',
                   '2026-07-01', '2026-06-30');" 2>&1 || true)
echo "$ERR" | grep -qi "check\|violates" \
  && pass "V036.4" "expires_at <= starts_at rejected" \
  || fail "V036.4" "backwards expires_at accepted: $ERR"

# V036.5: self-approval rejected (separation of duties)
ERR=$(sql "INSERT INTO privileged_access_approvals
             (session_id, company_id, approver_user_id, decision, reason)
           VALUES ('$SESS1', '$CO_ACME', '$U_ALICE', 'approved',
                   'approved by legal counsel');" 2>&1 || true)
echo "$ERR" | grep -qi "no_self_approval\|subject of the session" \
  && pass "V036.5" "self-approval rejected (separation of duties)" \
  || fail "V036.5" "self-approval accepted: $ERR"

# V036.6: approval by different user accepted
# We need another company user; U_ALICE is CO_ACME. Create a second Acme user.
U_CAROL='ca300001-0000-0000-0000-000000000000'
sql "INSERT INTO users (id, company_id, external_sub, email)
     VALUES ('$U_CAROL', '$CO_ACME', 'sub-carol', 'carol@acme.com');" > /dev/null

# Change session subject so U_ALICE can approve.
sql "INSERT INTO privileged_access_sessions
       (id, company_id, subject_user_id, requested_by, purpose, access_kind,
        starts_at, expires_at)
     VALUES ('$SESS2', '$CO_ACME', '$U_CAROL', '$U_CAROL',
             'Approved legal review for acquisition documents',
             'sensitive_analysis', now(), now() + INTERVAL '4 hours');" > /dev/null

sql "INSERT INTO privileged_access_approvals
       (session_id, company_id, approver_user_id, decision, reason)
     VALUES ('$SESS2', '$CO_ACME', '$U_ALICE', 'approved',
             'approved by legal counsel');" > /dev/null
COUNT=$(sql "SELECT count(*) FROM privileged_access_approvals
             WHERE session_id='$SESS2' AND decision='approved';")
[ "$COUNT" = "1" ] \
  && pass "V036.6" "approval by different user accepted" \
  || fail "V036.6" "cross-user approval rejected (count=$COUNT)"

# V036.7: approval UPDATE rejected (append-only)
ERR=$(sql "UPDATE privileged_access_approvals
           SET decision='rejected'
           WHERE session_id='$SESS2' AND approver_user_id='$U_ALICE';" 2>&1 || true)
echo "$ERR" | grep -qi "append-only\|append_only" \
  && pass "V036.7" "approval UPDATE rejected (append-only)" \
  || fail "V036.7" "approval UPDATE accepted: $ERR"

# V036.8: approval DELETE rejected (append-only)
ERR=$(sql "DELETE FROM privileged_access_approvals
           WHERE session_id='$SESS2' AND approver_user_id='$U_ALICE';" 2>&1 || true)
echo "$ERR" | grep -qi "append-only\|append_only" \
  && pass "V036.8" "approval DELETE rejected (append-only)" \
  || fail "V036.8" "approval DELETE accepted: $ERR"

# V036.9: privileged_access_scopes applies_to CHECK
ERR=$(sql "INSERT INTO privileged_access_scopes
             (session_id, company_id, org_unit_id, applies_to,
              maximum_sensitivity)
           VALUES ('$SESS2', '$CO_ACME', '$ACME_ROOT', 'bad_scope',
                   'restricted');" 2>&1 || true)
echo "$ERR" | grep -qi "applies_to_check\|check constraint" \
  && pass "V036.9" "scope applies_to CHECK rejects invalid" \
  || fail "V036.9" "invalid applies_to accepted: $ERR"

# V036.10: maximum_sensitivity CHECK rejects invalid
ERR=$(sql "INSERT INTO privileged_access_scopes
             (session_id, company_id, org_unit_id, applies_to,
              maximum_sensitivity)
           VALUES ('$SESS2', '$CO_ACME', '$ACME_ROOT', 'self_only',
                   'top_secret');" 2>&1 || true)
echo "$ERR" | grep -qi "maximum_sensitivity_check\|check constraint" \
  && pass "V036.10" "scope maximum_sensitivity CHECK rejects invalid" \
  || fail "V036.10" "invalid sensitivity accepted: $ERR"

# V036.11: valid scope accepted
sql "INSERT INTO privileged_access_scopes
       (session_id, company_id, org_unit_id, applies_to, maximum_sensitivity,
        allow_relationships, allow_impact_analysis, allow_diagnostics)
     VALUES ('$SESS2', '$CO_ACME', '$ACME_ROOT', 'self_and_descendants',
             'confidential', true, true, false);" > /dev/null
COUNT=$(sql "SELECT count(*) FROM privileged_access_scopes WHERE session_id='$SESS2';")
[ "$COUNT" = "1" ] \
  && pass "V036.11" "valid privileged_access_scope accepted" \
  || fail "V036.11" "valid scope rejected (count=$COUNT)"
