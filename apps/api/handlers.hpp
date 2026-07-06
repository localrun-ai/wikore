#pragma once
#include "wikore/rag/retrieval_orchestrator.hpp"
#include "wikore/rag/knowledge_edge_repo.hpp"
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <memory>
#include <string>

// ---------------------------------------------------------------------------
// HTTP handlers for the wikore API. Each handler is a coroutine returning a
// fully-built HttpResponsePtr; main.cpp registers it behind an AsyncTask
// adapter that invokes the drogon callback. Keeping the body in a
// Task<HttpResponsePtr> (rather than the AsyncTask itself) means the caller
// can catch a stray exception before it reaches AsyncTask, which would
// otherwise LOG_FATAL and abort the process.
// ---------------------------------------------------------------------------

namespace wikore::api {

// POST /api/orgs/{orgUnitId}/wiki/query - permission-aware retrieval.
//
// Runs the Iteration 2 read path (embed -> resolve reader scope -> derive
// clearance -> Qdrant prefilter -> vector search -> EvidenceGate) and returns
// the gated candidate chunks as JSON. The tenant is resolved from the
// authenticated user, never from the URL; orgUnitId only scopes the search
// and must belong to the caller's company.
drogon::Task<drogon::HttpResponsePtr>
wiki_query(std::shared_ptr<rag::RetrievalOrchestrator> orch,
           drogon::orm::DbClientPtr                    db,
           drogon::HttpRequestPtr                      req,
           std::string                                 org_unit_id);

// POST /api/orgs/{orgUnitId}/wiki/evidence - intent-dispatched retrieval.
//
// Sibling of wiki_query. Runs the Iteration-3 intent surface added in step 7
// (retrieve_evidence): Fact returns chunk-only variants (backward-compat
// shape); Bridge returns AllowedRelationship-only variants; Automatic
// interleaves both. Returns a discriminated response — each result carries
// a "kind" field ("chunk" or "relationship") so the client can render each
// alternative correctly without a schema-guessing pass.
//
// Bridge and Automatic require the orchestrator to have an edge Qdrant
// collection and RelationshipEvidenceGate wired up; Bridge fails closed with
// 503 when unwired, Automatic degrades to fact-only.
drogon::Task<drogon::HttpResponsePtr>
wiki_evidence(std::shared_ptr<rag::RetrievalOrchestrator> orch,
              drogon::orm::DbClientPtr                    db,
              drogon::HttpRequestPtr                      req,
              std::string                                 org_unit_id);

// GET /api/me - the authenticated caller's identity + tenant.
//
// Behind AuthFilter, so `identity` is present and its user_id is the internal
// users.id. Returns { user_id, email, display_name, is_admin, company_id }.
// company_id is looked up from the (active) user row; a deactivated user is
// rejected here too as defense in depth.
drogon::Task<drogon::HttpResponsePtr>
me(drogon::orm::DbClientPtr db, drogon::HttpRequestPtr req);

// GET /api/orgs/tree - the caller's company org-unit hierarchy.
//
// Behind AuthFilter. Resolves the tenant from the authenticated (active) user,
// then returns the full org_units tree for that company as nested nodes
// { id, type, slug, name, description, children }. Tenant-scoped; a deactivated
// user is rejected.
drogon::Task<drogon::HttpResponsePtr>
orgs_tree(drogon::orm::DbClientPtr db, drogon::HttpRequestPtr req);

// GET /api/orgs/{orgUnitId} - a single org unit's detail.
//
// Behind AuthFilter. Tenant resolved from the authenticated (active) user.
// Default-closed: a non-admin may read the unit only if it is within their
// membership scope; a miss (absent, other tenant, or no access) is 404 so
// existence never leaks. Admins may read any unit in their tenant. Returns
// { id, parent_id, type, slug, name, description }.
drogon::Task<drogon::HttpResponsePtr>
org_get(drogon::orm::DbClientPtr db, drogon::HttpRequestPtr req,
        std::string org_unit_id);

// -------------------- Admin: knowledge edges (BaryGraph Lite V034) --------
//
// All routes below sit behind BOTH AuthFilter and AdminFilter. Tenant is
// resolved from the authenticated (admin) user; the URL never carries a
// company_id. The KnowledgeEdgeRepo is passed in by shared_ptr from the
// startup wiring in apps/api/main.cpp so the handlers can be constructed
// lazily against the same DbClient used by the retrieval path.
//
// Endpoint immutability (V034 trigger) is respected at the type level:
// the update handler only exposes the mutable field set (confidence,
// review_state, provenance, expires_at). Adding a new endpoint requires
// deleting and recreating the edge.

// POST /api/admin/edges — create an admin-authored chunk-to-chunk edge.
drogon::Task<drogon::HttpResponsePtr>
edges_create(std::shared_ptr<rag::KnowledgeEdgeRepo> repo,
             drogon::orm::DbClientPtr                db,
             drogon::HttpRequestPtr                  req);

// GET /api/admin/edges/{edge_id} — single edge by id.
drogon::Task<drogon::HttpResponsePtr>
edges_get(std::shared_ptr<rag::KnowledgeEdgeRepo> repo,
          drogon::orm::DbClientPtr                db,
          drogon::HttpRequestPtr                  req,
          std::string                             edge_id);

// GET /api/admin/edges?edge_type=&review_state=&chunk_id=&limit=&offset=
drogon::Task<drogon::HttpResponsePtr>
edges_list(std::shared_ptr<rag::KnowledgeEdgeRepo> repo,
           drogon::orm::DbClientPtr                db,
           drogon::HttpRequestPtr                  req);

// PATCH /api/admin/edges/{edge_id} — update mutable fields only.
drogon::Task<drogon::HttpResponsePtr>
edges_update(std::shared_ptr<rag::KnowledgeEdgeRepo> repo,
             drogon::orm::DbClientPtr                db,
             drogon::HttpRequestPtr                  req,
             std::string                             edge_id);

// DELETE /api/admin/edges/{edge_id} — cascades to endpoints/embeddings,
// enqueues qdrant_delete_edge_points via the V034 BEFORE DELETE trigger.
drogon::Task<drogon::HttpResponsePtr>
edges_delete(std::shared_ptr<rag::KnowledgeEdgeRepo> repo,
             drogon::orm::DbClientPtr                db,
             drogon::HttpRequestPtr                  req,
             std::string                             edge_id);

} // namespace wikore::api
