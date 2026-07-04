#pragma once
#include "wikore/rag/retrieval_orchestrator.hpp"
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

} // namespace wikore::api
