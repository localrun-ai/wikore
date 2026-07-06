#include <catch2/catch_test_macros.hpp>
#include "handlers.hpp"                            // wikore::api::wiki_answer
#include "wikore/auth.hpp"                         // Identity
#include "wikore/rag/answer_use_case.hpp"
#include "wikore/rag/embedder.hpp"
#include "wikore/rag/vector_store.hpp"
#include "wikore/rag/relationship_vector_store.hpp"
#include "wikore/rag/evidence_gate.hpp"
#include "wikore/rag/relationship_evidence_gate.hpp"
#include "wikore/access_resolver.hpp"
#include "wikore/db.hpp"

#include <drogon/drogon.h>
#include <drogon/utils/coroutine.h>
#include <glaze/glaze.hpp>

#include <chrono>
#include <cstdlib>
#include <format>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Integration tests for POST /api/orgs/{orgUnitId}/wiki/answer (step 8c).
// Uses a stub LlmProviderPort so we can assert the wire response shape and
// citation IDs without a real upstream. The auth/tenant/scope pipeline
// runs against real Postgres (same helpers as /wiki/evidence). Skips
// without DATABASE_URL.
// ---------------------------------------------------------------------------

namespace {

constexpr auto CO = "05c00000-0000-0000-0000-0000000000f2"; // "answer-http co"
constexpr int  DIM = 4;

bool db_available() { return std::getenv("DATABASE_URL") != nullptr; }

template<typename... Args>
drogon::orm::Result exec_sync(drogon::orm::DbClientPtr db, std::string sql, Args... args)
{
    return drogon::sync_wait(
        [db, sql = std::move(sql), ...args = std::move(args)]()
        -> drogon::Task<drogon::orm::Result> {
            co_return co_await db->execSqlCoro(sql, args...);
        }());
}

std::string sync_scalar(drogon::orm::DbClientPtr db, std::string sql)
{
    auto rows = exec_sync(db, std::move(sql));
    return std::string(rows[0][0].c_str());
}

class StubLlm : public wikore::rag::LlmProviderPort {
public:
    void set_reply(std::string content) {
        std::lock_guard<std::mutex> lock(_m);
        _content = std::move(content);
    }
    struct Cap { std::string sys; std::string user; int calls = 0; };
    Cap captured() const {
        std::lock_guard<std::mutex> lock(_m);
        return _cap;
    }
    drogon::Task<wikore::Result<wikore::rag::ChatResponse>>
    chat(wikore::rag::ChatRequest req, double) const override
    {
        {
            std::lock_guard<std::mutex> lock(_m);
            for (const auto& m : req.messages) {
                if (m.role == "system") _cap.sys  = m.content;
                if (m.role == "user")   _cap.user = m.content;
            }
            ++_cap.calls;
        }
        co_return wikore::rag::ChatResponse{
            .content       = _content,
            .input_tokens  = 42,
            .output_tokens = 7,
            .model         = "stub-http-model",
            .provider_id   = "stub",
        };
    }
    drogon::Task<wikore::Result<wikore::rag::ChatResponse>>
    chat_stream(wikore::rag::ChatRequest,
                std::function<void(wikore::rag::ChatChunk)>,
                double) const override
    {
        co_return std::unexpected(wikore::Error::invalid_state("not impl"));
    }
private:
    mutable std::mutex _m;
    mutable Cap        _cap;
    std::string        _content = "stub answer citing [SRC 1]";
};

struct Fixture {
    std::string team;
    std::string user;
    std::string chunk_a;
    std::shared_ptr<wikore::rag::NullVectorStore>              chunk_store;
    std::shared_ptr<wikore::rag::NullRelationshipVectorStore>  edge_store;
    std::shared_ptr<StubLlm>                                    llm;
    std::shared_ptr<wikore::rag::AnswerUseCase>                use_case;
};

Fixture seed(drogon::orm::DbClientPtr db)
{
    exec_sync(db, "DELETE FROM companies WHERE id=$1::uuid", std::string(CO));
    exec_sync(db, "INSERT INTO companies (id,name,slug) VALUES ($1::uuid,'AnsH','ans-h')",
              std::string(CO));

    Fixture f;
    const auto root = sync_scalar(db, std::format(
        "SELECT id FROM org_units WHERE company_id='{}' AND type='root'", CO));
    f.team = sync_scalar(db, std::format(
        "INSERT INTO org_units (company_id,parent_id,type,slug,name) "
        "VALUES ('{}','{}','team','ans-h','Ans H') RETURNING id", CO, root));

    f.user = sync_scalar(db, std::format(
        "INSERT INTO users (company_id,external_sub,email) "
        "VALUES ('{}','sub-ans-h','ans-h@test') RETURNING id", CO));
    exec_sync(db, std::format(
        "INSERT INTO memberships (company_id,user_id,org_unit_id,role,applies_to) "
        "VALUES ('{}','{}','{}','viewer','self_and_descendants')",
        CO, f.user, f.team));

    const auto doc = sync_scalar(db, std::format(
        "INSERT INTO documents (company_id,owner_org_unit_id,filename,title,mime_type) "
        "VALUES ('{}','{}','h.txt','H','text/plain') RETURNING id", CO, f.team));
    const auto ver = sync_scalar(db, std::format(
        "INSERT INTO document_versions (company_id,document_id,version_no,source_hash,"
        "ingest_status,completed_at,activated_at,chunk_count,lifecycle_status,sensitivity_label) "
        "VALUES ('{}','{}',1,'h','done',now(),now(),1,'active','internal') RETURNING id",
        CO, doc));
    f.chunk_a = sync_scalar(db, std::format(
        "INSERT INTO document_chunks (company_id,document_version_id,chunk_index,"
        "content,content_hash,qdrant_prefilter_scope_ids) "
        "VALUES ('{}','{}',0,'chunk-A','h-a','{{}}') RETURNING id", CO, ver));

    wikore::rag::ChunkPayload p;
    p.company_id          = CO;
    p.document_version_id = ver;
    p.chunk_id            = f.chunk_a;
    p.access_scope_ids    = {f.team};
    p.sensitivity_label   = "internal";
    p.lifecycle_status    = "active";
    f.chunk_store = std::make_shared<wikore::rag::NullVectorStore>();
    drogon::sync_wait(f.chunk_store->upsert(std::vector<wikore::rag::UpsertPoint>{
        {.id = "pt-" + f.chunk_a,
         .vector = wikore::rag::Embedding(DIM, 0.1f),
         .payload = std::move(p)}}));

    f.edge_store = std::make_shared<wikore::rag::NullRelationshipVectorStore>();
    f.llm        = std::make_shared<StubLlm>();

    auto orch = std::make_shared<wikore::rag::RetrievalOrchestrator>(
        std::make_shared<wikore::rag::NullEmbedder>(DIM),
        std::make_shared<wikore::PostgresAccessResolver>(db),
        f.chunk_store,
        wikore::rag::EvidenceGate(db),
        f.edge_store,
        std::make_shared<wikore::rag::RelationshipEvidenceGate>(db));
    f.use_case = std::make_shared<wikore::rag::AnswerUseCase>(
        std::move(orch),
        std::make_shared<wikore::rag::ContextBuilder>(),
        f.llm);
    return f;
}

drogon::HttpRequestPtr make_req(const std::string& user, const std::string& body)
{
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Post);
    req->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    req->setBody(body);
    wikore::Identity id;
    id.user_id      = user;
    id.email        = "ans-h@test";
    id.display_name = "Ans H";
    req->getAttributes()->insert("identity", id);
    return req;
}

// Response DTO for parsing.
struct Usage { int input_tokens = 0; int output_tokens = 0; };
struct Cits  { std::vector<std::string> source_chunk_ids;
               std::vector<std::string> source_edge_ids; };
struct Resp  { std::string answer; Cits citations; std::string model;
               Usage usage; int evidence_included = 0; };

} // namespace

TEST_CASE("wiki_answer: happy path returns answer, citations, model, usage",
          "[integration][api][answer]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    auto f  = seed(db);

    auto req  = make_req(f.user,
        R"({"query":"what is chunk-a?","intent":"fact","limit":5})");
    auto resp = drogon::sync_wait(wikore::api::wiki_answer(
        f.use_case, db, req, f.team));

    REQUIRE(resp->getStatusCode() == drogon::k200OK);
    Resp parsed;
    REQUIRE_FALSE(glz::read<glz::opts{.error_on_unknown_keys = false}>(
        parsed, std::string(resp->getBody())));

    CHECK(parsed.answer == "stub answer citing [SRC 1]");
    CHECK(parsed.model  == "stub-http-model");
    CHECK(parsed.usage.input_tokens  == 42);
    CHECK(parsed.usage.output_tokens == 7);
    CHECK(parsed.citations.source_chunk_ids == std::vector<std::string>{f.chunk_a});
    CHECK(parsed.citations.source_edge_ids.empty());
    CHECK(parsed.evidence_included == 1);

    // LLM saw the default answer-semantics system prompt.
    const auto cap = f.llm->captured();
    CHECK(cap.calls == 1);
    CHECK(cap.sys.find("origin=llm_proposal") != std::string::npos);
    CHECK(cap.user.find("Question: what is chunk-a?") != std::string::npos);
}

TEST_CASE("wiki_answer: model override reaches the LLM request",
          "[integration][api][answer]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    auto f  = seed(db);

    // Provider gets the model override in ChatRequest.model — we can't
    // introspect it from the response (StubLlm always emits stub-http-model),
    // but the request must succeed and the LLM must have been called.
    auto req  = make_req(f.user,
        R"({"query":"q","intent":"fact","model":"gpt-override"})");
    auto resp = drogon::sync_wait(wikore::api::wiki_answer(
        f.use_case, db, req, f.team));
    REQUIRE(resp->getStatusCode() == drogon::k200OK);
    CHECK(f.llm->captured().calls == 1);
}

TEST_CASE("wiki_answer: caller-supplied system prompt overrides the default",
          "[integration][api][answer]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    auto f  = seed(db);

    auto req = make_req(f.user,
        R"({"query":"q","intent":"fact","system_prompt":"be brief"})");
    auto resp = drogon::sync_wait(wikore::api::wiki_answer(
        f.use_case, db, req, f.team));
    REQUIRE(resp->getStatusCode() == drogon::k200OK);
    const auto cap = f.llm->captured();
    CHECK(cap.sys == "be brief");
}

TEST_CASE("wiki_answer: empty system_prompt string reaches the LLM verbatim",
          "[integration][api][answer]")
{
    // P3 contract from #61 review: HTTP-side empty string means
    // "no system message" (bare prompt for evaluation harnesses),
    // NOT "use the default". The whole point of splitting the
    // optional override from context_opts is to make empty distinct
    // from absent.
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    auto f  = seed(db);

    auto req = make_req(f.user,
        R"({"query":"q","intent":"fact","system_prompt":""})");
    auto resp = drogon::sync_wait(wikore::api::wiki_answer(
        f.use_case, db, req, f.team));
    REQUIRE(resp->getStatusCode() == drogon::k200OK);
    const auto cap = f.llm->captured();
    CHECK(cap.sys.empty());
    CHECK(cap.sys.find("origin=llm_proposal") == std::string::npos);
}

TEST_CASE("wiki_answer: input validation (400 / 401 / 404)",
          "[integration][api][answer]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    auto f  = seed(db);

    SECTION("unknown intent -> 400") {
        auto resp = drogon::sync_wait(wikore::api::wiki_answer(
            f.use_case, db,
            make_req(f.user, R"({"query":"q","intent":"strange"})"),
            f.team));
        CHECK(resp->getStatusCode() == drogon::k400BadRequest);
    }
    SECTION("empty query -> 400") {
        auto resp = drogon::sync_wait(wikore::api::wiki_answer(
            f.use_case, db,
            make_req(f.user, R"({"query":"    "})"), f.team));
        CHECK(resp->getStatusCode() == drogon::k400BadRequest);
    }
    SECTION("non-positive limit -> 400") {
        auto resp = drogon::sync_wait(wikore::api::wiki_answer(
            f.use_case, db,
            make_req(f.user, R"({"query":"q","limit":0})"), f.team));
        CHECK(resp->getStatusCode() == drogon::k400BadRequest);
    }
    SECTION("out-of-range min_confidence -> 400") {
        auto resp = drogon::sync_wait(wikore::api::wiki_answer(
            f.use_case, db,
            make_req(f.user,
                R"({"query":"q","intent":"bridge","min_confidence":2.0})"),
            f.team));
        CHECK(resp->getStatusCode() == drogon::k400BadRequest);
    }
    SECTION("empty allowed_review_states -> 400") {
        auto resp = drogon::sync_wait(wikore::api::wiki_answer(
            f.use_case, db,
            make_req(f.user,
                R"({"query":"q","intent":"bridge","allowed_review_states":[]})"),
            f.team));
        CHECK(resp->getStatusCode() == drogon::k400BadRequest);
    }
    SECTION("non-uuid org unit -> 404") {
        auto resp = drogon::sync_wait(wikore::api::wiki_answer(
            f.use_case, db,
            make_req(f.user, R"({"query":"q"})"), "not-a-uuid"));
        CHECK(resp->getStatusCode() == drogon::k404NotFound);
    }
    SECTION("missing identity -> 401") {
        auto req = drogon::HttpRequest::newHttpRequest();
        req->setMethod(drogon::Post);
        req->setBody(R"({"query":"q"})");
        auto resp = drogon::sync_wait(wikore::api::wiki_answer(
            f.use_case, db, req, f.team));
        CHECK(resp->getStatusCode() == drogon::k401Unauthorized);
    }

    // On any validation failure, the LLM must not have been called.
    CHECK(f.llm->captured().calls == 0);
}
