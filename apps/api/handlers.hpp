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

} // namespace wikore::api
