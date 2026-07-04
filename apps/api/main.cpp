#include "wikore/config.hpp"
#include "wikore/auth.hpp"
#include "wikore/db.hpp"
#include "wikore/redis.hpp"
#include "wikore/org_tree.hpp"   // OrgUnit, OrgTreeService, Company
#include "wikore/access.hpp"    // AccessService, Role
#include "wikore/access_resolver.hpp"           // Postgres/CachedAccessResolver
#include "wikore/rag/embedder.hpp"              // LlamaEmbedder
#include "wikore/rag/vector_store.hpp"          // QdrantVectorStore
#include "wikore/rag/evidence_gate.hpp"         // EvidenceGate
#include "wikore/rag/retrieval_orchestrator.hpp"
#include "handlers.hpp"

#include <drogon/drogon.h>
#include <spdlog/spdlog.h>
#include <format>
#include <memory>

// ---------------------------------------------------------------------------
// Route stubs - each will move to its own handler file as implemented.
// All handlers are coroutines; filters applied per-group.
// ---------------------------------------------------------------------------

using Req = drogon::HttpRequestPtr;
using CB  = std::function<void(const drogon::HttpResponsePtr&)>;

static drogon::HttpResponsePtr json_ok(std::string body) {
    auto r = drogon::HttpResponse::newHttpResponse();
    r->setStatusCode(drogon::k200OK);
    r->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    r->setBody(std::move(body));
    return r;
}

static drogon::HttpResponsePtr not_implemented() {
    auto r = drogon::HttpResponse::newHttpResponse();
    r->setStatusCode(drogon::k501NotImplemented);
    r->setBody(R"({"error":"not implemented"})");
    r->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    return r;
}

int main() {
    const auto cfg = wikore::Config::from_env();

    spdlog::info("[wikore] commit:  {}", WIKORE_GIT_HASH);
    spdlog::info("[wikore] port:    {}", cfg.port);
    spdlog::info("[wikore] db:      {}", cfg.database_url);
    spdlog::info("[wikore] qdrant:  {}", cfg.qdrant_url);
    spdlog::info("[wikore] llm:     {}", cfg.llm_base_url);

    // Register Drogon DB client before run() so getDbClient() works in filters.
    wikore::Db::init(cfg);
    wikore::Redis::init(cfg);
    // Fetch OIDC JWKS synchronously - Drogon event loop not yet started here,
    // so we use a direct SSL BIO call inside auth_init.
    wikore::auth_init(cfg);

    // -----------------------------------------------------------------------
    // RAG read-path singletons. Constructed lazily inside a beginning-advice
    // callback (below) rather than here: wikore::Db::get() returns a live
    // DbClient only after drogon::app().run() has materialized the pool, so
    // touching it at main scope asserts. The handler captures this holder by
    // shared_ptr; beginning advice runs on the loop before any listener
    // accepts a connection, so `orch` is always populated by request time.
    // -----------------------------------------------------------------------
    struct RagDeps {
        std::shared_ptr<wikore::rag::RetrievalOrchestrator> orch;
        drogon::orm::DbClientPtr                            db;
    };
    auto deps = std::make_shared<RagDeps>();

    drogon::app().registerBeginningAdvice([deps, cfg]() {
        auto db = wikore::Db::get();
        auto embedder = std::make_shared<wikore::rag::LlamaEmbedder>(
            cfg.embed_base_url, cfg.embed_model, cfg.embed_dims);
        auto resolver = std::make_shared<wikore::CachedAccessResolver>(
            std::make_shared<wikore::PostgresAccessResolver>(db), db);
        auto vector_store =
            std::make_shared<wikore::rag::QdrantVectorStore>(cfg.qdrant_url);
        deps->db   = db;
        deps->orch = std::make_shared<wikore::rag::RetrievalOrchestrator>(
            embedder, resolver, vector_store, wikore::rag::EvidenceGate(db));
        spdlog::info("[wikore] RAG read path ready");
    });

    // -----------------------------------------------------------------------
    // Health (public)
    // -----------------------------------------------------------------------
    drogon::app().registerHandler("/api/health",
        [](const Req&, CB&& cb) {
            cb(json_ok(R"({"ok":true})"));
        }, {drogon::Get});

    // -----------------------------------------------------------------------
    // Identity
    // -----------------------------------------------------------------------
    drogon::app().registerHandler("/api/me",
        [deps](Req req, CB cb) -> drogon::AsyncTask {
            if (!deps->db) {   // before beginning advice populated it
                auto r = drogon::HttpResponse::newHttpResponse();
                r->setStatusCode(drogon::k503ServiceUnavailable);
                r->setContentTypeCode(drogon::CT_APPLICATION_JSON);
                r->setBody(R"({"error":"service starting"})");
                cb(r);
                co_return;
            }
            auto resp = co_await wikore::api::me(deps->db, std::move(req));
            cb(resp);
        },
        {drogon::Get, "wikore::AuthFilter"});

    // -----------------------------------------------------------------------
    // Org tree
    // -----------------------------------------------------------------------
    drogon::app().registerHandler("/api/orgs/tree",
        [deps](Req req, CB cb) -> drogon::AsyncTask {
            if (!deps->db) {   // before beginning advice populated it
                auto r = drogon::HttpResponse::newHttpResponse();
                r->setStatusCode(drogon::k503ServiceUnavailable);
                r->setContentTypeCode(drogon::CT_APPLICATION_JSON);
                r->setBody(R"({"error":"service starting"})");
                cb(r);
                co_return;
            }
            auto resp = co_await wikore::api::orgs_tree(deps->db, std::move(req));
            cb(resp);
        },
        {drogon::Get, "wikore::AuthFilter"});

    drogon::app().registerHandler("/api/orgs",
        [](const Req&, CB&& cb) { cb(not_implemented()); },
        {drogon::Post, "wikore::AuthFilter"});

    drogon::app().registerHandler("/api/orgs/{1}",
        [deps](Req req, CB cb, std::string org_unit_id) -> drogon::AsyncTask {
            if (!deps->db) {   // before beginning advice populated it
                auto r = drogon::HttpResponse::newHttpResponse();
                r->setStatusCode(drogon::k503ServiceUnavailable);
                r->setContentTypeCode(drogon::CT_APPLICATION_JSON);
                r->setBody(R"({"error":"service starting"})");
                cb(r);
                co_return;
            }
            auto resp = co_await wikore::api::org_get(deps->db, std::move(req),
                                                      std::move(org_unit_id));
            cb(resp);
        },
        {drogon::Get, "wikore::AuthFilter"});
    // PATCH / DELETE of an org unit are not implemented yet.
    drogon::app().registerHandler("/api/orgs/{1}",
        [](const Req&, CB&& cb, std::string) { cb(not_implemented()); },
        {drogon::HttpMethod::Patch, drogon::Delete, "wikore::AuthFilter"});

    // Members
    drogon::app().registerHandler("/api/orgs/{1}/members",
        [](const Req&, CB&& cb, std::string) { cb(not_implemented()); },
        {drogon::Get, drogon::Post, "wikore::AuthFilter"});

    drogon::app().registerHandler("/api/orgs/{1}/members/{2}",
        [](const Req&, CB&& cb, std::string, std::string) { cb(not_implemented()); },
        {drogon::HttpMethod::Patch, drogon::Delete, "wikore::AuthFilter"});

    // Cross-org grants
    drogon::app().registerHandler("/api/orgs/{1}/grants",
        [](const Req&, CB&& cb, std::string) { cb(not_implemented()); },
        {drogon::Get, drogon::Post, "wikore::AuthFilter"});

    drogon::app().registerHandler("/api/orgs/{1}/grants/{2}",
        [](const Req&, CB&& cb, std::string, std::string) { cb(not_implemented()); },
        {drogon::Delete, "wikore::AuthFilter"});

    // -----------------------------------------------------------------------
    // Documents / RAG ingest
    // -----------------------------------------------------------------------
    drogon::app().registerHandler("/api/orgs/{1}/docs",
        [](const Req&, CB&& cb, std::string) { cb(not_implemented()); },
        {drogon::Get, drogon::Post, "wikore::AuthFilter"});

    drogon::app().registerHandler("/api/orgs/{1}/docs/{2}",
        [](const Req&, CB&& cb, std::string, std::string) { cb(not_implemented()); },
        {drogon::Get, drogon::Delete, "wikore::AuthFilter"});

    // -----------------------------------------------------------------------
    // Chat (SSE)
    // -----------------------------------------------------------------------
    drogon::app().registerHandler("/api/orgs/{1}/chat/stream",
        [](const Req&, CB&& cb, std::string) { cb(not_implemented()); },
        {drogon::Post, "wikore::AuthFilter"});

    drogon::app().registerHandler("/api/orgs/{1}/chat/sessions",
        [](const Req&, CB&& cb, std::string) { cb(not_implemented()); },
        {drogon::Get, "wikore::AuthFilter"});

    drogon::app().registerHandler("/api/orgs/{1}/chat/sessions/{2}",
        [](const Req&, CB&& cb, std::string, std::string) { cb(not_implemented()); },
        {drogon::Get, drogon::Delete, "wikore::AuthFilter"});

    // -----------------------------------------------------------------------
    // Wiki
    // -----------------------------------------------------------------------
    drogon::app().registerHandler("/api/orgs/{1}/wiki/pages",
        [](const Req&, CB&& cb, std::string) { cb(not_implemented()); },
        {drogon::Get, "wikore::AuthFilter"});

    drogon::app().registerHandler("/api/orgs/{1}/wiki/pages/{2}",
        [](const Req&, CB&& cb, std::string, std::string) { cb(not_implemented()); },
        {drogon::Get, drogon::Delete, "wikore::AuthFilter"});

    drogon::app().registerHandler("/api/orgs/{1}/wiki/ingest",
        [](const Req&, CB&& cb, std::string) { cb(not_implemented()); },
        {drogon::Post, "wikore::AuthFilter"});

    drogon::app().registerHandler("/api/orgs/{1}/wiki/query",
        [deps](Req req, CB cb, std::string org_unit_id) -> drogon::AsyncTask {
            // req/cb taken by value so they live in the coroutine frame across
            // the co_await (drogon's calling frame is gone after the first
            // suspension). wiki_query never throws - it converts every failure
            // into a response - so nothing escapes this AsyncTask.
            if (!deps->orch) {          // before beginning advice populated it
                auto r = drogon::HttpResponse::newHttpResponse();
                r->setStatusCode(drogon::k503ServiceUnavailable);
                r->setContentTypeCode(drogon::CT_APPLICATION_JSON);
                r->setBody(R"({"error":"service starting"})");
                cb(r);
                co_return;
            }
            auto resp = co_await wikore::api::wiki_query(
                deps->orch, deps->db, std::move(req), std::move(org_unit_id));
            cb(resp);
        },
        {drogon::Post, "wikore::AuthFilter"});

    drogon::app().registerHandler("/api/orgs/{1}/wiki/lint",
        [](const Req&, CB&& cb, std::string) { cb(not_implemented()); },
        {drogon::Post, "wikore::AuthFilter"});

    // -----------------------------------------------------------------------
    // Integrations / MCP
    // -----------------------------------------------------------------------
    drogon::app().registerHandler("/api/orgs/{1}/integrations",
        [](const Req&, CB&& cb, std::string) { cb(not_implemented()); },
        {drogon::Get, drogon::Post, "wikore::AuthFilter"});

    drogon::app().registerHandler("/api/orgs/{1}/integrations/{2}",
        [](const Req&, CB&& cb, std::string, std::string) { cb(not_implemented()); },
        {drogon::Get, drogon::HttpMethod::Patch, drogon::Delete, "wikore::AuthFilter"});

    // -----------------------------------------------------------------------
    // Admin
    // -----------------------------------------------------------------------
    drogon::app().registerHandler("/api/admin/audit",
        [](const Req&, CB&& cb) { cb(not_implemented()); },
        {drogon::Get, "wikore::AuthFilter", "wikore::AdminFilter"});

    drogon::app().registerHandler("/api/admin/users",
        [](const Req&, CB&& cb) { cb(not_implemented()); },
        {drogon::Get, drogon::Post, "wikore::AuthFilter", "wikore::AdminFilter"});

    // -----------------------------------------------------------------------
    // Server config
    // -----------------------------------------------------------------------
    drogon::app()
        .setLogPath("./")
        .setLogLevel(trantor::Logger::kWarn)
        .addListener("0.0.0.0", cfg.port)
        .setThreadNum(static_cast<int>(std::thread::hardware_concurrency()))
        .setIdleConnectionTimeout(120)
        .run();
}
