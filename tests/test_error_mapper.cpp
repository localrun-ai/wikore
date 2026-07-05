#include <catch2/catch_test_macros.hpp>
#include "wikore/adapters/postgres/error_mapper_internal.hpp"
#include "wikore/db.hpp"
#include <drogon/orm/DbClient.h>
#include <drogon/orm/Exception.h>
#include <drogon/utils/coroutine.h>
#include <cstdlib>
#include <string>
#include <unordered_set>
#include <vector>

// Test the constraint-name extraction logic in pg_error_mapper.
// Uses the production extract_constraint() via error_mapper_internal.hpp so
// any change to the regex is immediately caught here.

using wikore::postgres::detail::extract_constraint;

TEST_CASE("extract_constraint: unique violation message", "[error_mapper]") {
    const std::string msg =
        R"(ERROR: duplicate key value violates unique constraint "document_versions_one_active_per_doc_uidx")"
        "\nDETAIL: Key (company_id, document_id)=(...) already exists.";
    REQUIRE(extract_constraint(msg) == "document_versions_one_active_per_doc_uidx");
}

TEST_CASE("extract_constraint: check violation message", "[error_mapper]") {
    const std::string msg =
        R"(ERROR: new row for relation "document_versions" violates check constraint "document_versions_active_state_chk")";
    REQUIRE(extract_constraint(msg) == "document_versions_active_state_chk");
}

TEST_CASE("extract_constraint: FK violation message", "[error_mapper]") {
    const std::string msg =
        R"(ERROR: insert or update on table "memberships" violates foreign key constraint "memberships_user_same_company_fk")";
    REQUIRE(extract_constraint(msg) == "memberships_user_same_company_fk");
}

TEST_CASE("extract_constraint: no constraint in message returns empty", "[error_mapper]") {
    const std::string msg = "ERROR: connection to server lost";
    REQUIRE(extract_constraint(msg).empty());
}

TEST_CASE("extract_constraint: outbox idempotency key constraint", "[error_mapper]") {
    const std::string msg =
        R"(ERROR: duplicate key value violates unique constraint "outbox_events_company_id_job_type_idempotency_key_key")";
    REQUIRE(extract_constraint(msg) == "outbox_events_company_id_job_type_idempotency_key_key");
}

TEST_CASE("extract_constraint: org_unit self-loop check", "[error_mapper]") {
    // Note: this verifies the regex extraction only. The constraint name
    // 'org_unit_self_loop_chk' is not in the V001-V030 schema (org-tree
    // self-loops are prevented by org_units_no_self_parent_trigger, not a
    // CHECK constraint), so this string would surface as "unmapped" through
    // map_db_exception. The introspection test below pins that fact.
    const std::string msg =
        R"(ERROR: new row for relation "org_units" violates check constraint "org_unit_self_loop_chk")";
    REQUIRE(extract_constraint(msg) == "org_unit_self_loop_chk");
}

// ---------------------------------------------------------------------------
// Coverage / introspection: assert the constraint map keeps up with the schema.
//
// The map under test (error_mapper.cpp:k_constraint_map) maps named CHECK and
// UNIQUE constraints to typed domain Errors. When a migration adds a new
// constraint, the corresponding violation otherwise falls through to
// Error::database_error -> opaque 500 instead of a typed 409/400 the HTTP
// layer can render. This test introspects the live schema and asserts every
// constraint is either mapped or in an explicit allow-list, so the drift is
// caught at PR time rather than discovered in production.
//
// Allow-list policy: constraints whose violation is genuinely unactionable to
// the caller (e.g., FK same-company guards which the application is already
// expected to satisfy via tenant isolation, never-null columns whose absence
// is a clear caller bug) are listed by name with a brief justification.
// ---------------------------------------------------------------------------

namespace {

// Constraints that are intentionally NOT in k_constraint_map. Each entry must
// have a justification comment alongside it.
const std::unordered_set<std::string> k_allowlist = {
    // Composite-FK target UNIQUE keys: every tenant-scoped table carries
    // `UNIQUE (company_id, ...)` so the matching FK can reference
    // `(company_id, ...)` and make cross-tenant references impossible at the
    // DB layer. A violation here means the caller stitched a cross-tenant
    // reference, which is a programming bug. Default database_error -> 500 is
    // the correct response (we want it loud).
    "chat_sessions_company_id_id_key",
    "document_chunks_company_id_document_version_id_id_key",
    "document_chunks_company_id_id_key",
    "document_sections_company_id_document_version_id_id_key",
    "document_sections_company_id_id_key",
    "document_versions_company_id_document_id_id_key",
    "document_versions_company_id_id_key",
    "documents_company_id_id_key",
    "groups_company_id_id_key",
    "org_units_company_id_id_key",
    "prompt_templates_company_id_id_key",
    "users_company_id_id_key",
    "wiki_page_versions_company_id_id_key",
    "wiki_pages_company_id_id_key",

    // Temporal-history infrastructure UNIQUE keys: emitted by the
    // generated history triggers. The application never INSERTs into the
    // history tables directly; a violation indicates an internal bug.
    "memberships_history_live_row_id_valid_from_key",
    "resource_grants_history_live_row_id_valid_from_key",

    // FK same-company guards: a violation here means the caller stitched a
    // cross-tenant reference, which is a programming bug. The default
    // database_error -> 500 is the correct response (we want it loud).
    "api_keys_user_same_company_fk",
    "chat_sessions_org_unit_same_company_fk",
    "chat_sessions_user_same_company_fk",
    "chat_turns_parent_fk",
    "chat_turns_prompt_template_same_company_fk",
    "chat_turns_session_same_company_fk",
    "chunk_vectors_chunk_same_company_fk",
    "chunks_version_same_company_fk",
    "closure_ancestor_same_company_fk",
    "closure_descendant_same_company_fk",
    "cqs_chunk_same_company_fk",
    "ctf_turn_same_company_fk",
    "ctf_user_same_company_fk",
    "document_sections_parent_same_version_fk",
    "document_sections_version_same_company_fk",
    "document_versions_document_same_company_fk",
    "documents_owner_same_company_fk",
    "group_members_group_same_company_fk",
    "group_members_user_same_company_fk",
    "integrations_org_unit_same_company_fk",
    "memberships_group_same_company_fk",
    "memberships_org_unit_same_company_fk",
    "memberships_user_same_company_fk",
    "org_units_parent_same_company_fk",
    "shared_chat_revoker_fk",
    "shared_chat_session_same_company_fk",
    "shared_chat_sharer_same_company_fk",
    "wiki_links_org_unit_same_company_fk",
    "wiki_page_versions_page_same_company_fk",
    "wiki_pages_org_unit_same_company_fk",
    "wps_chunk_belongs_to_version_fk",
    "wps_document_same_company_fk",
    "wps_version_belongs_to_document_fk",
    "wps_wiki_page_version_same_company_fk",

    // V033: llm_providers
    // The same-company trigger raises as foreign_key_violation with a named
    // constraint; treated as a programming bug -> database_error.
    "llm_providers_created_by_same_company_fk",

    // V034: knowledge_edges (BaryGraph Lite)
    // Composite-FK target UNIQUE key (programming bug -> database_error).
    "knowledge_edges_company_id_id_key",
    // Endpoint (edge_id, chunk_id) UNIQUE — a duplicate chunk endpoint on
    // the same edge is a programming bug.
    "knowledge_edge_endpoints_edge_id_chunk_id_key",
    // (embedding_model_id, qdrant_point_id) UNIQUE — a duplicate Qdrant
    // point ID for the same model is a programming bug (uuid_v5 collision).
    // Explicit name because PG truncates auto-generated names at 63 chars.
    "knowledge_edge_embeddings_point_uniq",
    // History (live_row_id, valid_from) UNIQUE — clock_timestamp collision
    // is a programming bug.
    "knowledge_edges_history_live_row_id_valid_from_key",
    // Partial unique index on open history intervals — a duplicate open
    // interval indicates a snapshot ordering bug.
    "knowledge_edges_history_open_uidx",
    // Same-company triggers raise as foreign_key_violation with named
    // constraints; treated as programming bugs.
    "knowledge_edges_created_by_same_company_fk",
    "knowledge_edges_reviewed_by_same_company_fk",

    // V035: capability grants
    // The (expires_at > granted_at) and (revoked_at, revoked_by) CHECKs are
    // named as capability_grants_check_1/2 by PG (unnamed inline CHECKs).
    // Allow-listed because inserting an inconsistent grant is a programming
    // bug in the entitlement service.
    "user_capability_grants_check",
    "user_capability_grants_check1",
    "group_capability_grants_check",
    "group_capability_grants_check1",
    "org_unit_capability_grants_check",
    "org_unit_capability_grants_check1",
    // Composite same-company FK for tenant_features.enabled_by raises as
    // foreign_key_violation with a named constraint; a cross-tenant actor
    // is a programming bug -> database_error.
    "tenant_features_enabled_by_same_company_fk",

    // V036: privileged access sessions
    // Inline CHECKs (expires_at > starts_at, revoked_at/revoked_by pair
    // consistency) get PG-generated names. Allowed-listed because
    // inserting an inconsistent session is a programming bug in the
    // session-management service.
    "privileged_access_sessions_check",
    "privileged_access_sessions_check1",
    // Composite-FK target UNIQUE key on (company_id, id) — auto-named by PG.
    // A duplicate would require a duplicate primary-key id, which the PK
    // constraint already rejects. Programming bug -> database_error.
    "privileged_access_sessions_company_id_id_key",

    // V037: edge vector indexing
    // Empty vector CHECK — inserting a zero-dim vector is a bug in the
    // admin embed job, not user input.
    "edge_type_vectors_vector_dim_positive_chk",
    // Inline description length CHECK — same rationale.
    "edge_type_vectors_check",
    // PK on (formula_version, edge_type, embedding_model_id) — a
    // duplicate is a bug in the admin embed job (should upsert instead
    // of inserting twice).
    "edge_type_vectors_pkey",
    // Internal denormalized counter maintained by the approval trigger.
    "privileged_access_sessions_decision_count_chk",
    // History change_kind is trigger-owned, not caller input.
    "privileged_access_session_history_change_kind_check",
};

bool integration_db_available() {
    return std::getenv("DATABASE_URL") != nullptr;
}

drogon::orm::DbClientPtr admin_db() {
    return wikore::Db::get();
}

} // namespace

TEST_CASE("pg_constraint introspection: every CHECK/UNIQUE in public schema is mapped or allow-listed",
          "[integration][error_mapper]")
{
    if (!integration_db_available()) SKIP("DATABASE_URL not set");
    auto db = admin_db();

    // Discover every CHECK and UNIQUE constraint on public-schema tables.
    // coninhcount=0 dedupes constraints that PostgreSQL has propagated to
    // partition children (every partition inherits the parent's CHECK, but
    // there is only one constraint to map). FK constraints are not in the
    // map; the SQLSTATE 23503 branch handles them, and only the
    // same-company guards are interesting (see allowlist).
    //
    // CREATE UNIQUE INDEX ... <name> (without a UNIQUE constraint) is not
    // recorded in pg_constraint but its violation message still includes
    // `constraint "<index_name>"`. We surface those via pg_indexes so the
    // map covers both UNIQUE constraints AND constraint-less unique
    // indexes such as the partial `*_uidx` predicates.
    std::vector<std::string> unmapped;
    drogon::sync_wait([&]() -> drogon::Task<void> {
        try {
            auto rows = co_await db->execSqlCoro(R"(
                SELECT con.conname AS name
                FROM   pg_constraint con
                JOIN   pg_class      rel ON rel.oid = con.conrelid
                JOIN   pg_namespace  ns  ON ns.oid  = rel.relnamespace
                WHERE  ns.nspname        = 'public'
                  AND  con.contype       IN ('c','u')
                  AND  con.coninhcount   = 0
                UNION
                SELECT con.conname AS name
                FROM   pg_constraint con
                JOIN   pg_class      rel ON rel.oid = con.conrelid
                JOIN   pg_namespace  ns  ON ns.oid  = rel.relnamespace
                WHERE  ns.nspname        = 'public'
                  AND  con.contype       = 'f'
                  AND  con.coninhcount   = 0
                  AND  con.conname LIKE '%_same_company_fk'
                UNION
                SELECT i.indexname AS name
                FROM   pg_indexes i
                LEFT JOIN pg_constraint c
                       ON c.conname      = i.indexname
                      AND c.connamespace = (
                          SELECT oid FROM pg_namespace WHERE nspname = i.schemaname)
                WHERE  i.schemaname = 'public'
                  AND  c.oid IS NULL
                  AND  i.indexdef LIKE 'CREATE UNIQUE INDEX%'
            )");
            for (const auto& r : rows) {
                const auto name = r["name"].as<std::string>();
                if (wikore::postgres::detail::is_constraint_mapped(name)) continue;
                if (k_allowlist.contains(name)) continue;
                unmapped.push_back(name);
            }
        } catch (const drogon::orm::DrogonDbException& ex) {
            FAIL("pg_constraint introspection failed: " << ex.base().what());
        }
    }());

    if (!unmapped.empty()) {
        std::string detail;
        for (const auto& n : unmapped) detail += "\n  - " + n;
        FAIL("The following constraints are neither in k_constraint_map nor "
             "in the allow-list. Add a typed Error mapping in "
             "src/adapters/postgres/error_mapper.cpp or add to the "
             "allow-list with justification in test_error_mapper.cpp:"
             << detail);
    }
}
