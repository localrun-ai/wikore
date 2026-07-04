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
sql "INSERT INTO privileged_access_scopes
       (session_id, company_id, org_unit_id, applies_to, maximum_sensitivity)
     VALUES ('$SESS1', '$CO_ACME', '$ACME_ROOT', 'self_only', 'confidential');" > /dev/null
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

# Define the complete request before recording a decision. Scope becomes
# immutable as soon as the first approval/rejection is inserted.
sql "INSERT INTO privileged_access_scopes
       (session_id, company_id, org_unit_id, applies_to, maximum_sensitivity,
        allow_relationships, allow_impact_analysis, allow_diagnostics)
     VALUES ('$SESS2', '$CO_ACME', '$ACME_ROOT', 'self_and_descendants',
             'confidential', true, true, false);" > /dev/null

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
           VALUES ('$SESS1', '$CO_ACME', '$ACME_ROOT', 'bad_scope',
                   'restricted');" 2>&1 || true)
echo "$ERR" | grep -qi "applies_to_check\|check constraint" \
  && pass "V036.9" "scope applies_to CHECK rejects invalid" \
  || fail "V036.9" "invalid applies_to accepted: $ERR"

# V036.10: maximum_sensitivity CHECK rejects invalid
ERR=$(sql "INSERT INTO privileged_access_scopes
             (session_id, company_id, org_unit_id, applies_to,
              maximum_sensitivity)
           VALUES ('$SESS1', '$CO_ACME', '$ACME_ROOT', 'self_only',
                   'top_secret');" 2>&1 || true)
echo "$ERR" | grep -qi "maximum_sensitivity_check\|check constraint" \
  && pass "V036.10" "scope maximum_sensitivity CHECK rejects invalid" \
  || fail "V036.10" "invalid sensitivity accepted: $ERR"

# V036.11: valid scope accepted
COUNT=$(sql "SELECT count(*) FROM privileged_access_scopes WHERE session_id='$SESS2';")
[ "$COUNT" = "1" ] \
  && pass "V036.11" "valid privileged_access_scope accepted" \
  || fail "V036.11" "valid scope rejected (count=$COUNT)"

# V036.12: cross-tenant approval REJECTED (P1 regression).
# Try to attach a company-Beta approval row to an Acme session — the
# composite (company_id, session_id) FK must reject it because no
# (BETA, SESS2) row exists in privileged_access_sessions.
ERR=$(sql "INSERT INTO privileged_access_approvals
             (session_id, company_id, approver_user_id, decision, reason)
           VALUES ('$SESS2', '$CO_BETA', '$U_BOB', 'approved',
                   'cross-tenant approval attempt');" 2>&1 || true)
echo "$ERR" | grep -qi "foreign key\|violates\|not present in table" \
  && pass "V036.12" "cross-tenant approval rejected by composite FK" \
  || fail "V036.12" "cross-tenant approval accepted: $ERR"

# V036.13: cross-tenant scope REJECTED (P1 regression).
ERR=$(sql "INSERT INTO privileged_access_scopes
             (session_id, company_id, org_unit_id, applies_to, maximum_sensitivity)
           VALUES ('$SESS2', '$CO_BETA', '$BETA_ROOT', 'self_only',
                   'confidential');" 2>&1 || true)
echo "$ERR" | grep -qi "foreign key\|violates\|not present in table" \
  && pass "V036.13" "cross-tenant scope rejected by composite FK" \
  || fail "V036.13" "cross-tenant scope accepted: $ERR"

# V036.14: dual-approval trigger uses the renamed constraint
# (P3 rename: previously *_dual_approval_requires_two -> *_no_requester_approval).
DUAL_SESS='6a550003-0000-0000-0000-000000000003'
sql "INSERT INTO privileged_access_sessions
       (id, company_id, subject_user_id, requested_by, purpose, access_kind,
        starts_at, expires_at, require_dual_approval)
     VALUES ('$DUAL_SESS', '$CO_ACME', '$U_CAROL', '$U_ALICE',
             'Approved dual-approval acquisition review',
             'sensitive_analysis', now(), now() + INTERVAL '4 hours',
             true);" > /dev/null
sql "INSERT INTO privileged_access_scopes
       (session_id, company_id, org_unit_id, applies_to, maximum_sensitivity)
     VALUES ('$DUAL_SESS', '$CO_ACME', '$ACME_ROOT', 'self_only',
             'confidential');" > /dev/null
ERR=$(sql "INSERT INTO privileged_access_approvals
             (session_id, company_id, approver_user_id, decision, reason)
           VALUES ('$DUAL_SESS', '$CO_ACME', '$U_ALICE', 'approved',
                   'requester self-approving');" 2>&1 || true)
echo "$ERR" | grep -qi "no_requester_approval\|requester cannot approve" \
  && pass "V036.14" "dual-approval trigger uses renamed constraint" \
  || fail "V036.14" "unexpected error text: $ERR"

# V036.15: retroactive UPDATE of subject_user_id after approval REJECTED
# (closes the loophole where separation-of-duties can be defeated post-hoc).
ERR=$(sql "UPDATE privileged_access_sessions
           SET subject_user_id='$U_ALICE'
           WHERE id='$SESS2';" 2>&1 || true)
echo "$ERR" | grep -qi "request_immutable_after_decision\|request fields are immutable" \
  && pass "V036.15" "subject_user_id UPDATE after approval rejected" \
  || fail "V036.15" "retroactive subject swap accepted: $ERR"

# V036.16: retroactive UPDATE of requested_by after approval REJECTED
ERR=$(sql "UPDATE privileged_access_sessions
           SET requested_by='$U_ALICE'
           WHERE id='$SESS2';" 2>&1 || true)
echo "$ERR" | grep -qi "request_immutable_after_decision\|request fields are immutable" \
  && pass "V036.16" "requested_by UPDATE after approval rejected" \
  || fail "V036.16" "retroactive requester swap accepted: $ERR"

# V036.17: validated pending -> approved workflow progression works
sql "UPDATE privileged_access_sessions
     SET status='approved'
     WHERE id='$SESS2';" > /dev/null
STATUS=$(sql "SELECT status FROM privileged_access_sessions WHERE id='$SESS2';")
[ "$STATUS" = "approved" ] \
  && pass "V036.17" "approved transition accepted with sufficient approval" \
  || fail "V036.17" "workflow progression blocked (status=$STATUS)"

# V036.18: subject/requester UPDATE on a session with NO approvals still works
NOAPPR_SESS='6a550004-0000-0000-0000-000000000004'
sql "INSERT INTO privileged_access_sessions
       (id, company_id, subject_user_id, requested_by, purpose, access_kind,
        starts_at, expires_at)
     VALUES ('$NOAPPR_SESS', '$CO_ACME', '$U_ALICE', '$U_ALICE',
             'Draft engagement, no approvals yet',
             'temporary_engagement', now(), now() + INTERVAL '1 hour');" > /dev/null
sql "UPDATE privileged_access_sessions
     SET subject_user_id='$U_CAROL', requested_by='$U_CAROL'
     WHERE id='$NOAPPR_SESS';" > /dev/null
COUNT=$(sql "SELECT count(*) FROM privileged_access_sessions
             WHERE id='$NOAPPR_SESS' AND subject_user_id='$U_CAROL';")
[ "$COUNT" = "1" ] \
  && pass "V036.18" "subject/requester UPDATE allowed on session with no approvals" \
  || fail "V036.18" "no-approval subject swap wrongly rejected"

# V036.19: pending -> active bypass rejected even when there are no approvals.
ERR=$(sql "UPDATE privileged_access_sessions SET status='active'
           WHERE id='$NOAPPR_SESS';" 2>&1 || true)
echo "$ERR" | grep -qi "legal_transition\|illegal privileged_access transition" \
  && pass "V036.19" "pending -> active workflow bypass rejected" \
  || fail "V036.19" "pending -> active bypass accepted: $ERR"

# V036.20: the approved request cannot be expanded after approval.
ERR=$(sql "UPDATE privileged_access_sessions
           SET allow_llm=true, expires_at=expires_at + INTERVAL '1 day'
           WHERE id='$SESS2';" 2>&1 || true)
echo "$ERR" | grep -qi "request_immutable_after_decision\|request fields are immutable" \
  && pass "V036.20" "post-approval request expansion rejected" \
  || fail "V036.20" "post-approval request expansion accepted: $ERR"

# V036.21: approved scopes cannot be expanded or erased.
ERR=$(sql "UPDATE privileged_access_scopes
           SET maximum_sensitivity='restricted', allow_diagnostics=true
           WHERE session_id='$SESS2';" 2>&1 || true)
echo "$ERR" | grep -qi "scopes_immutable_after_decision\|scopes are immutable" \
  && pass "V036.21" "post-approval scope expansion rejected" \
  || fail "V036.21" "post-approval scope expansion accepted: $ERR"

# V036.22: activation is accepted only after the approved transition and the
# trigger supplies activated_at itself.
sql "UPDATE privileged_access_sessions SET status='active' WHERE id='$SESS2';" > /dev/null
COUNT=$(sql "SELECT count(*) FROM privileged_access_sessions
             WHERE id='$SESS2' AND status='active' AND activated_at IS NOT NULL;")
[ "$COUNT" = "1" ] \
  && pass "V036.22" "approved session activated with trigger timestamp" \
  || fail "V036.22" "valid activation failed (count=$COUNT)"

# V036.23: active cannot return to an earlier workflow state.
ERR=$(sql "UPDATE privileged_access_sessions SET status='approved'
           WHERE id='$SESS2';" 2>&1 || true)
echo "$ERR" | grep -qi "legal_transition\|illegal privileged_access transition" \
  && pass "V036.23" "backwards workflow transition rejected" \
  || fail "V036.23" "backwards workflow transition accepted: $ERR"

# V036.24: every state transition has an immutable request + scope snapshot.
COUNT=$(sql "SELECT count(*) FROM privileged_access_session_history
             WHERE live_session_id='$SESS2'
               AND new_status IN ('pending','approved','active')
               AND jsonb_array_length(scope_snapshot) > 0;")
# The initial pending snapshot precedes scope creation; approved and active do
# include it, so exactly two matching transition snapshots are expected.
[ "$COUNT" = "2" ] \
  && pass "V036.24" "transition history captures approved scope snapshots" \
  || fail "V036.24" "transition scope history incomplete (count=$COUNT)"

# V036.25: transition history is append-only.
ERR=$(sql "DELETE FROM privileged_access_session_history
           WHERE live_session_id='$SESS2';" 2>&1 || true)
echo "$ERR" | grep -qi "append-only\|append_only" \
  && pass "V036.25" "transition history DELETE rejected" \
  || fail "V036.25" "transition history DELETE accepted: $ERR"

# V036.26: durable privileged records explicitly block ordinary tenant purge.
PURGE_CO='ca7e0036-0000-0000-0000-000000000000'
PURGE_USER='ca7e0036-0000-0000-0000-000000000001'
sql "INSERT INTO companies (id,name,slug)
     VALUES ('$PURGE_CO','V036 retention','v036-retention');" > /dev/null
sql "INSERT INTO users (id,company_id,external_sub,email)
     VALUES ('$PURGE_USER','$PURGE_CO','v036-retention-user','retention@example.test');" > /dev/null
sql "INSERT INTO privileged_access_sessions
       (company_id,subject_user_id,requested_by,purpose,access_kind,starts_at,expires_at)
     VALUES ('$PURGE_CO','$PURGE_USER','$PURGE_USER','Durable security record',
             'temporary_engagement',now(),now()+INTERVAL '1 hour');" > /dev/null
ERR=$(sql "DELETE FROM companies WHERE id='$PURGE_CO';" 2>&1 || true)
echo "$ERR" | grep -qi "foreign key\|violates\|still referenced" \
  && pass "V036.26" "ordinary tenant purge cannot erase privileged history" \
  || fail "V036.26" "tenant purge unexpectedly erased privileged record: $ERR"

# V036.27-V036.28: dual approval is enforced by the transition itself.
U_DAVE='da7e0036-0000-0000-0000-000000000001'
U_ERIN='e17e0036-0000-0000-0000-000000000001'
sql "INSERT INTO users (id,company_id,external_sub,email) VALUES
       ('$U_DAVE','$CO_ACME','sub-dave','dave@acme.com'),
       ('$U_ERIN','$CO_ACME','sub-erin','erin@acme.com');" > /dev/null
sql "INSERT INTO privileged_access_approvals
       (session_id,company_id,approver_user_id,decision,reason)
     VALUES ('$DUAL_SESS','$CO_ACME','$U_DAVE','approved','first independent approver');" > /dev/null
ERR=$(sql "UPDATE privileged_access_sessions SET status='approved'
           WHERE id='$DUAL_SESS';" 2>&1 || true)
echo "$ERR" | grep -qi "approval_requirement\|needs 2 approvals" \
  && pass "V036.27" "one approval cannot advance a dual-approval session" \
  || fail "V036.27" "dual-approval session advanced with one approval: $ERR"
sql "INSERT INTO privileged_access_approvals
       (session_id,company_id,approver_user_id,decision,reason)
     VALUES ('$DUAL_SESS','$CO_ACME','$U_ERIN','approved','second independent approver');" > /dev/null
sql "UPDATE privileged_access_sessions SET status='approved'
     WHERE id='$DUAL_SESS';" > /dev/null
STATUS=$(sql "SELECT status FROM privileged_access_sessions WHERE id='$DUAL_SESS';")
[ "$STATUS" = "approved" ] \
  && pass "V036.28" "two independent approvals advance dual-approval session" \
  || fail "V036.28" "dual-approval session did not advance (status=$STATUS)"
