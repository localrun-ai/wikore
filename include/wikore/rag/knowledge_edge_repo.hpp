#pragma once
#include "wikore/domain/knowledge_edge.hpp"
#include "wikore/domain/types.hpp"
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wikore::rag {

// ---------------------------------------------------------------------------
// KnowledgeEdgeRepo — Postgres adapter for the V034 knowledge_edges tables.
//
// Every method is tenant-scoped: company_id from the RequestContext is
// carried into the WHERE clause of every read and the INSERT of every
// write. V034's composite FKs additionally enforce that endpoints (and
// their chunks) belong to the caller's tenant.
//
// Endpoint immutability (V034 trigger) is respected by never issuing an
// UPDATE against knowledge_edge_endpoints; the update() method touches
// knowledge_edges only.
//
// Callers should always route through this class (or a use case above it)
// rather than issuing raw SQL, so that:
//   * the CTE-based two-endpoint insert (which synchronously commits at
//     statement level and clears the deferred CONSTRAINT TRIGGER) stays
//     in one place;
//   * error mapping goes through map_db_exception so V034's named CHECK/
//     trigger constraints turn into typed Errors the API layer can render.
// ---------------------------------------------------------------------------

class KnowledgeEdgeRepo {
public:
    explicit KnowledgeEdgeRepo(drogon::orm::DbClientPtr db) : db_(std::move(db)) {}

    // Insert one knowledge_edge and its two endpoints in a single
    // data-modifying CTE (WITH e AS (INSERT ...) INSERT ... FROM e).
    // A single statement is used because:
    //   * it runs in its own implicit transaction, so the deferred
    //     two-endpoint CONSTRAINT TRIGGER fires at statement commit and
    //     any failure surfaces synchronously;
    //   * drogon under pipeline mode (PgBatchConnection) routes every
    //     query through a prepared statement, and prepared statements
    //     reject multi-command strings ("cannot insert multiple commands
    //     into a prepared statement");
    //   * exec_sync-style return means the write is durable before
    //     control returns, so a following read sees the row.
    drogon::Task<Result<domain::KnowledgeEdge>>
    create(std::string_view                     company_id,
           const Uuid&                          actor_user_id,   // request principal
           const domain::CreateKnowledgeEdgeCmd& cmd);

    // Fetch a single edge with its endpoints. Tenant-scoped; a miss
    // (absent, or a different tenant) returns Error::not_found —
    // existence never leaks.
    drogon::Task<Result<domain::KnowledgeEdge>>
    get(std::string_view company_id, std::string_view edge_id);

    // List edges with optional filters. Ordered by created_at DESC, id ASC
    // for stable pagination. limit/offset are already validated by the caller.
    drogon::Task<Result<std::vector<domain::KnowledgeEdge>>>
    list(std::string_view                        company_id,
         const domain::ListKnowledgeEdgesFilter& filter);

    // Update a subset of the mutable fields (confidence, review_state,
    // provenance, expires_at). Endpoints, edge_type, direction, and origin
    // are IMMUTABLE — attempting to change them is a caller-side error and
    // callers must not surface such requests here (the UpdateCmd shape
    // enforces that at the type level). If review_state moves to
    // accepted/rejected/superseded, this method also stamps reviewed_by
    // and reviewed_at automatically.
    //
    // NOTE: the AFTER UPDATE trigger on knowledge_edges snapshots the new
    // state into knowledge_edges_history for us; nothing to do here.
    drogon::Task<Result<domain::KnowledgeEdge>>
    update(std::string_view                      company_id,
           const Uuid&                           actor_user_id,
           std::string_view                      edge_id,
           const domain::UpdateKnowledgeEdgeCmd& cmd);

    // Delete an edge. The V034 BEFORE DELETE trigger captures every
    // Qdrant point ID into an outbox event (job_type =
    // qdrant_delete_edge_points) before ON DELETE CASCADE removes the
    // knowledge_edge_embeddings child rows; the EdgeVectorCleanupWorker
    // (PR #53) drains that outbox. Callers do NOT need to touch Qdrant.
    drogon::Task<Result<void>>
    remove(std::string_view company_id, std::string_view edge_id);

private:
    drogon::orm::DbClientPtr db_;
};

} // namespace wikore::rag
