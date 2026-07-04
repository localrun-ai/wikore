#include "wikore/adapters/postgres/error_mapper.hpp"
#include "wikore/adapters/postgres/error_mapper_internal.hpp"
#include <drogon/orm/Exception.h>
#include <unordered_map>
#include <string>
#include <string_view>

namespace wikore::postgres {

namespace {

constexpr std::string_view SQLSTATE_SERIALIZATION_FAILURE  = "40001";
constexpr std::string_view SQLSTATE_DEADLOCK               = "40P01";
constexpr std::string_view SQLSTATE_FK_VIOLATION           = "23503";
constexpr std::string_view SQLSTATE_NOT_NULL_VIOLATION     = "23502";
constexpr std::string_view SQLSTATE_QUERY_CANCELED         = "57014";

// Maps named CHECK / UNIQUE constraints (and unique-index constraint names) to
// typed domain Errors. The pg_constraint introspection test
// (test_error_mapper.cpp) asserts that every public CHECK constraint and every
// constraint-backed unique index is either present in this map or in the
// explicit allow-list, so the map cannot silently drift as migrations are
// added.
const std::unordered_map<std::string, Error> k_constraint_map = {
    // -----------------------------------------------------------------------
    // V001: org units
    // -----------------------------------------------------------------------
    {"org_units_one_root_per_company_uidx",
        Error::conflict("company already has a root org unit")},
    {"org_units_root_parent_shape_chk",
        Error::invalid_input("root org unit must have NULL parent_id; non-root must have a parent")},

    // -----------------------------------------------------------------------
    // V002: memberships and resource grants
    // -----------------------------------------------------------------------
    {"memberships_user_org_uidx",
        Error::conflict("membership already exists for this user and org unit")},
    {"memberships_group_org_uidx",
        Error::conflict("membership already exists for this group and org unit")},
    {"memberships_history_open_uidx",
        Error::conflict("at most one open membership per (user|group, org_unit) is allowed")},
    {"memberships_exactly_one_principal",
        Error::invalid_input("membership must reference exactly one of user_id or group_id")},
    {"memberships_expires_after_granted_chk",
        Error::invalid_input("membership expires_at must be after granted_at")},
    {"rg_resource_applies_to_valid",
        Error::invalid_input("resource_applies_to must be 'self_only' or 'self_and_descendants'")},
    {"rg_principal_applies_to_valid",
        Error::invalid_input("principal_applies_to must be 'self_only' or 'self_and_descendants'")},
    {"resource_grants_history_open_uidx",
        Error::conflict("at most one open grant per (resource, principal, permission) is allowed")},

    // -----------------------------------------------------------------------
    // V003: documents / document_versions
    // -----------------------------------------------------------------------
    {"document_versions_one_active_per_doc_uidx",
        Error::conflict("document already has an active version")},
    {"document_versions_active_state_chk",
        Error::invalid_state("active version must have completed ingest and activated_at")},
    {"document_versions_done_state_chk",
        Error::invalid_state("done ingest must have completed_at and chunk_count")},
    {"document_versions_active_requires_done_chk",
        Error::invalid_state("active lifecycle requires completed ingest")},
    {"document_versions_superseded_after_activated_chk",
        Error::invalid_state("superseded_at must be after activated_at")},
    {"document_versions_deprecated_interval_chk",
        Error::invalid_state("deprecated version with activated_at must have superseded_at")},
    {"document_versions_ingest_retry_count_chk",
        Error::invalid_input("ingest_retry_count must be non-negative")},

    // -----------------------------------------------------------------------
    // V005: chat turns
    // -----------------------------------------------------------------------
    {"chat_turns_rag_sources_array_chk",
        Error::invalid_state("rag_sources must be a JSON array")},
    {"chat_turns_tool_calls_array_chk",
        Error::invalid_state("tool_calls must be a JSON array")},

    // -----------------------------------------------------------------------
    // V014: sensitivity labels + wiki page versions
    // -----------------------------------------------------------------------
    {"document_versions_sensitivity_label_check",
        Error::invalid_input("sensitivity_label must be public, internal, confidential, or restricted")},
    {"documents_sensitivity_label_default_check",
        Error::invalid_input("sensitivity_label_default must be public, internal, confidential, or restricted")},
    {"wiki_page_versions_active_state_chk",
        Error::invalid_state("active wiki page version must have activated_at")},
    {"wiki_page_versions_deprecated_interval_chk",
        Error::invalid_state("deprecated wiki page version with activated_at must have superseded_at")},
    {"wiki_page_versions_superseded_after_activated_chk",
        Error::invalid_state("superseded_at must be after activated_at")},
    {"wiki_page_versions_one_active_uidx",
        Error::conflict("wiki page already has an active version")},

    // -----------------------------------------------------------------------
    // V015: outbox idempotency
    // -----------------------------------------------------------------------
    {"outbox_events_company_id_job_type_idempotency_key_key",
        Error::conflict("outbox event with this idempotency key already exists")},

    // -----------------------------------------------------------------------
    // V017+: groups / integrations / shared_chat
    // -----------------------------------------------------------------------
    {"groups_external_both_or_neither",
        Error::invalid_input("groups external_directory and external_id must be both set or both null")},
    {"groups_external_uidx",
        Error::conflict("groups external (directory, id) must be unique per company")},
    {"integrations_credentials_key_id_consistent_chk",
        Error::invalid_input("integration credential key id consistency violation")},
    {"shared_chat_revoke_consistent_chk",
        Error::invalid_input("shared_chat revocation must set both revoked_at and revoked_by, or neither")},

    // V033: llm_providers
    {"llm_providers_credentials_key_id_consistent_chk",
        Error::invalid_input("llm_provider credential key id consistency violation")},
    {"llm_providers_azure_requires_base_url_chk",
        Error::invalid_input("azure_openai provider requires base_url and azure_api_version")},
    {"llm_providers_openai_compat_requires_base_url_chk",
        Error::invalid_input("openai_compatible provider requires base_url")},
    {"llm_providers_max_tokens_check",
        Error::invalid_input("llm_provider max_tokens must be between 1 and 131072")},
    {"llm_providers_temperature_check",
        Error::invalid_input("llm_provider temperature must be between 0 and 2")},
    {"llm_providers_provider_check",
        Error::invalid_input("llm_provider type must be openai_compatible, azure_openai, anthropic, or gemini")},
    // A duplicate default for the same company scope is an admin conflict, not
    // a programming bug: maps to 409 so callers get an actionable error.
    {"llm_providers_one_default_per_scope_idx",
        Error::conflict("an active default LLM provider is already configured for this scope")},

    // V034: knowledge_edges (BaryGraph Lite)
    {"knowledge_edges_direction_check",
        Error::invalid_input("knowledge_edge direction must be 'directed' or 'symmetric'")},
    {"knowledge_edges_confidence_check",
        Error::invalid_input("knowledge_edge confidence must be between 0 and 1")},
    {"knowledge_edges_origin_check",
        Error::invalid_input("knowledge_edge origin must be 'parser', 'deterministic_rule', 'administrator', or 'llm_proposal'")},
    {"knowledge_edges_review_state_check",
        Error::invalid_input("knowledge_edge review_state must be 'accepted', 'proposed', 'rejected', or 'superseded'")},
    {"knowledge_edges_edge_type_v1_chk",
        Error::invalid_input("knowledge_edge edge_type not in V1 vocabulary (chunk-to-chunk types only)")},
    {"knowledge_edges_check",
        Error::invalid_input("knowledge_edge expires_at must be greater than created_at")},
    {"knowledge_edge_endpoints_ordinal_check",
        Error::invalid_input("knowledge_edge_endpoint ordinal must be 0 or 1")},
    {"knowledge_edge_endpoints_role_check",
        Error::invalid_input("knowledge_edge_endpoint role must be 'source', 'target', 'subject', 'object', 'a', or 'b'")},
    {"knowledge_edges_history_change_kind_check",
        Error::invalid_input("knowledge_edges_history change_kind must be 'insert', 'update', or 'delete'")},
    {"knowledge_edges_history_check",
        Error::invalid_input("knowledge_edges_history valid_until must be greater than or equal to valid_from")},
    {"knowledge_edges_exactly_two_endpoints_chk",
        Error::invalid_state("knowledge_edge must have exactly two endpoints at COMMIT")},
    // Endpoint immutability trigger surfaces as insufficient_privilege with
    // a named constraint; treated as invalid_state (programming bug).
    {"knowledge_edge_endpoints_immutable",
        Error::invalid_state("knowledge_edge_endpoints is immutable; delete the edge and recreate it instead")},

    // -----------------------------------------------------------------------
    // Enum / domain CHECKs (input validation): PG-generated names from
    // `column ... CHECK (column = ANY (...))`. Each maps to invalid_input.
    // -----------------------------------------------------------------------
    {"api_keys_role_check",
        Error::invalid_input("api_key role must be 'viewer', 'editor', or 'admin'")},
    {"chat_turn_feedback_signal_check",
        Error::invalid_input("chat_turn_feedback signal must be -1 or 1")},
    {"document_versions_ingest_status_check",
        Error::invalid_input("ingest_status must be 'pending', 'processing', 'done', or 'error'")},
    {"document_versions_lifecycle_status_check",
        Error::invalid_input("lifecycle_status must be 'draft', 'active', 'deprecated', or 'archived'")},
    {"integrations_type_check",
        Error::invalid_input("integration type must be one of the supported provider names")},
    {"memberships_applies_to_check",
        Error::invalid_input("membership applies_to must be 'self_only' or 'self_and_descendants'")},
    {"memberships_history_change_kind_check",
        Error::invalid_input("membership history change_kind must be 'insert', 'update', or 'delete'")},
    {"memberships_role_check",
        Error::invalid_input("membership role must be 'viewer', 'editor', or 'admin'")},
    {"org_units_type_check",
        Error::invalid_input("org_unit type must be one of 'root', 'subsidiary', 'division', 'department', 'team', 'project'")},
    {"resource_grants_history_change_kind_check",
        Error::invalid_input("grant history change_kind must be 'insert', 'update', or 'delete'")},
    {"resource_grants_permission_check",
        Error::invalid_input("grant permission must be 'read', 'write', or 'admin'")},
    {"resource_grants_principal_applies_to_check",
        Error::invalid_input("grant principal_applies_to must be 'self_only' or 'self_and_descendants'")},
    {"resource_grants_principal_type_check",
        Error::invalid_input("grant principal_type must be 'org_unit'")},
    {"resource_grants_resource_applies_to_check",
        Error::invalid_input("grant resource_applies_to must be 'self_only' or 'self_and_descendants'")},
    {"resource_grants_resource_type_check",
        Error::invalid_input("grant resource_type must be 'org_unit', 'document', or 'wiki_page'")},
    {"usage_events_event_type_check",
        Error::invalid_input("usage_event event_type must be 'llm_chat', 'llm_embed', or 'llm_rerank'")},
    {"wiki_page_versions_lifecycle_status_check",
        Error::invalid_input("wiki page version lifecycle_status must be 'draft', 'active', 'deprecated', or 'archived'")},
    {"wiki_page_versions_sensitivity_label_check",
        Error::invalid_input("wiki page version sensitivity_label must be public, internal, confidential, or restricted")},
    {"wiki_pages_lifecycle_status_check",
        Error::invalid_input("wiki page lifecycle_status must be 'draft', 'active', 'deprecated', or 'archived'")},
    {"wiki_pages_sensitivity_label_default_check",
        Error::invalid_input("wiki page sensitivity_label_default must be public, internal, confidential, or restricted")},

    // -----------------------------------------------------------------------
    // Numeric-range CHECKs (input validation).
    // -----------------------------------------------------------------------
    {"chat_turns_latency_ms_check",
        Error::invalid_input("chat_turns latency_ms must be non-negative")},
    {"chat_turns_tokens_used_check",
        Error::invalid_input("chat_turns tokens_used must be non-negative")},
    {"chunk_quality_signals_negative_count_check",
        Error::invalid_input("chunk_quality_signals negative_count must be non-negative")},
    {"chunk_quality_signals_positive_count_check",
        Error::invalid_input("chunk_quality_signals positive_count must be non-negative")},
    {"companies_default_retention_days_check",
        Error::invalid_input("companies default_retention_days must be positive")},
    {"document_sections_depth_check",
        Error::invalid_input("document_sections depth must be non-negative")},
    {"document_versions_chunk_count_check",
        Error::invalid_input("document_versions chunk_count must be non-negative")},
    {"document_versions_size_bytes_check",
        Error::invalid_input("document_versions size_bytes must be non-negative")},
    {"document_versions_version_no_check",
        Error::invalid_input("document_versions version_no must be positive")},
    {"documents_authority_level_check",
        Error::invalid_input("documents authority_level must be between 0 and 100")},
    {"embedding_models_dimension_check",
        Error::invalid_input("embedding_models dimension must be positive")},
    {"org_unit_closure_depth_check",
        Error::invalid_input("org_unit_closure depth must be non-negative")},
    {"usage_events_cost_micros_check",
        Error::invalid_input("usage_events cost_micros must be non-negative")},
    {"usage_events_latency_ms_check",
        Error::invalid_input("usage_events latency_ms must be non-negative")},
    {"usage_events_tokens_in_check",
        Error::invalid_input("usage_events tokens_in must be non-negative")},
    {"usage_events_tokens_out_check",
        Error::invalid_input("usage_events tokens_out must be non-negative")},
    {"wiki_page_versions_version_no_check",
        Error::invalid_input("wiki_page_versions version_no must be positive")},

    // -----------------------------------------------------------------------
    // resource_grants_check is the PG-generated name for the inline
    // CHECK (expires_at IS NULL OR expires_at > granted_at) on the table.
    // -----------------------------------------------------------------------
    {"resource_grants_check",
        Error::invalid_input("grant expires_at must be after granted_at")},

    // -----------------------------------------------------------------------
    // UNIQUE / conflict (409). Composite-FK target keys (company_id+id) are
    // in the test allow-list; the keys below are the user-facing ones.
    // -----------------------------------------------------------------------
    {"api_keys_key_hash_key",
        Error::conflict("api key with this hash already exists")},
    {"chat_turn_feedback_chat_turn_id_user_id_key",
        Error::conflict("feedback from this user for this turn already exists")},
    {"chat_turns_company_id_uniq",
        Error::conflict("chat_turns row with this (company_id, id) already exists")},
    {"companies_slug_key",
        Error::conflict("company slug already in use")},
    {"document_chunk_vectors_embedding_model_id_qdrant_point_id_key",
        Error::conflict("a Qdrant point with this (embedding_model_id, qdrant_point_id) is already bookkept")},
    {"document_chunks_document_version_id_chunk_index_key",
        Error::conflict("chunk_index already exists for this document_version")},
    {"document_sections_company_id_document_version_id_ordinal_key",
        Error::conflict("section ordinal already exists for this document_version")},
    {"document_versions_company_id_document_id_version_no_key",
        Error::conflict("document already has a version with this version_no")},
    {"embedding_models_name_key",
        Error::conflict("embedding_models name already in use")},
    {"embedding_models_qdrant_collection_key",
        Error::conflict("embedding_models qdrant_collection already in use")},
    {"groups_company_id_name_key",
        Error::conflict("group name already in use within this company")},
    {"integrations_org_unit_id_type_name_key",
        Error::conflict("integration with this (type, name) already exists for this org_unit")},
    {"mcp_tools_integration_id_tool_name_key",
        Error::conflict("MCP tool with this name already exists for this integration")},
    {"org_units_parent_slug_uidx",
        Error::conflict("org unit slug must be unique within its parent")},
    {"prompt_templates_company_id_name_content_hash_key",
        Error::conflict("prompt template with this (name, content_hash) already exists")},
    {"resource_grants_company_id_resource_type_resource_id_princi_key",
        Error::conflict("a grant with this (resource, principal, permission) already exists")},
    {"shared_chat_threads_share_token_key",
        Error::conflict("share_token already in use")},
    {"users_company_id_email_key",
        Error::conflict("user with this email already exists in this company")},
    {"users_company_id_external_issuer_external_sub_key",
        Error::conflict("user with this (external_issuer, external_sub) already exists in this company")},
    {"wiki_page_versions_company_id_wiki_page_id_version_no_key",
        Error::conflict("wiki page already has a version with this version_no")},
    {"wiki_pages_org_unit_id_slug_key",
        Error::conflict("wiki page slug already in use within this org_unit")},

    // -----------------------------------------------------------------------
    // V025/V026: wiki page sources (partial unique indexes).
    // -----------------------------------------------------------------------
    {"wiki_page_sources_version_unique_idx",
        Error::conflict("wiki page version is already linked to this document version (without chunk)")},
    {"wiki_page_sources_chunk_unique_idx",
        Error::conflict("wiki page version is already linked to this (document version, chunk)")},
};

} // namespace

Error map_db_exception(const drogon::orm::DrogonDbException& ex) {
    // A timeout (drogon's client-level SQL execution timeout, or exec_until
    // rejecting an already-expired deadline) is a request-budget failure, not a
    // server error -> ServiceUnavailable (503).
    if (dynamic_cast<const drogon::orm::TimeoutError*>(&ex))
        return Error::unavailable("query timed out (deadline exceeded)");

    // DrogonDbException is not std::exception; use .base() then dynamic_cast
    // to SqlError to access SQLSTATE.
    const auto* sql_err = dynamic_cast<const drogon::orm::SqlError*>(&ex.base());

    const std::string msg        = ex.base().what();
    const std::string sqlstate   = sql_err ? sql_err->sqlState() : "";
    const std::string constraint = detail::extract_constraint(msg);

    // Query cancelled - in this codebase that is a statement_timeout fired by a
    // deadline-bounded query (postgres::exec_until). Surface it as
    // ServiceUnavailable so the API returns 503 (deadline), not 500. A timeout
    // that fires inside a transaction is torn down and reaches us as a plain
    // DrogonDbException (not a SqlError, so no SQLSTATE), so also match the
    // stable Postgres message.
    if (sqlstate == SQLSTATE_QUERY_CANCELED
        || msg.find("canceling statement due to statement timeout") != std::string::npos)
        return Error::unavailable("query cancelled (deadline exceeded)");

    if (sqlstate == SQLSTATE_SERIALIZATION_FAILURE || sqlstate == SQLSTATE_DEADLOCK)
        return Error::conflict("transaction conflict, retry: " + msg);

    if (!constraint.empty()) {
        auto it = k_constraint_map.find(constraint);
        if (it != k_constraint_map.end())
            return it->second;
        return Error::database_error("unmapped constraint [" + constraint + "]: " + msg);
    }

    if (sqlstate == SQLSTATE_FK_VIOLATION)
        return Error::invalid_input("referenced entity does not exist or belongs to a different company");

    if (sqlstate == SQLSTATE_NOT_NULL_VIOLATION)
        return Error::invalid_input("required field is missing: " + msg);

    return Error::database_error("database error [" + sqlstate + "]: " + msg);
}

namespace detail {
bool is_constraint_mapped(std::string_view name) {
    return k_constraint_map.find(std::string(name)) != k_constraint_map.end();
}
} // namespace detail

} // namespace wikore::postgres
