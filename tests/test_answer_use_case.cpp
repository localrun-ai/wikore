#include <catch2/catch_test_macros.hpp>
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

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <format>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Integration tests for AnswerUseCase (BaryGraph Lite step 8c).
//
// Uses a stub LlmProviderPort that captures the ChatRequest and returns a
// canned response so we can assert the composition: retrieve_evidence →
// ContextBuilder → LlmProvider is wired correctly, the answer-semantics
// system prompt is applied by default, the citation IDs surface, and the
// deadline is threaded through the LLM call.
//
// The retrieval half runs against a real Postgres so the gate SQL and the
// origin plumbing (V034 → HydratedRow → AllowedRelationship → prompt) are
// exercised end-to-end. Skips without DATABASE_URL.
// ---------------------------------------------------------------------------

namespace {

constexpr auto CO  = "05c00000-0000-0000-0000-0000000000f1"; // "answer co"
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

// Stub LlmProviderPort — records the last request and returns a canned
// response. Thread-safe capture so we can assert what the prompt looked
// like after ContextBuilder rendered it.
class StubLlm : public wikore::rag::LlmProviderPort {
public:
    // Set what chat() should return. If .content is empty and the
    // .error is set, chat() returns that error instead.
    struct Reply {
        std::string             content = "The answer per [SRC 1] is 42.";
        int                     input_tokens  = 100;
        int                     output_tokens = 20;
        std::string             model         = "stub-model-1";
        std::optional<wikore::Error> error;
    };
    void set_reply(Reply r) {
        std::lock_guard<std::mutex> lock(_m);
        _reply = std::move(r);
    }

    struct Captured {
        std::vector<wikore::rag::ChatMessage> messages;
        std::string                            model;
        double                                 timeout_s = 0.0;
        int                                    calls     = 0;
    };
    Captured captured() const {
        std::lock_guard<std::mutex> lock(_m);
        return _captured;
    }

    drogon::Task<wikore::Result<wikore::rag::ChatResponse>>
    chat(wikore::rag::ChatRequest req, double timeout_s) const override
    {
        Reply reply;
        {
            std::lock_guard<std::mutex> lock(_m);
            _captured.messages  = req.messages;
            _captured.model     = req.model;
            _captured.timeout_s = timeout_s;
            ++_captured.calls;
            reply = _reply;
        }
        if (reply.error)
            co_return std::unexpected(*reply.error);
        co_return wikore::rag::ChatResponse{
            .content       = std::move(reply.content),
            .input_tokens  = reply.input_tokens,
            .output_tokens = reply.output_tokens,
            .model         = std::move(reply.model),
            .provider_id   = "stub",
        };
    }
    drogon::Task<wikore::Result<wikore::rag::ChatResponse>>
    chat_stream(wikore::rag::ChatRequest,
                std::function<void(wikore::rag::ChatChunk)>,
                double) const override
    {
        co_return std::unexpected(
            wikore::Error::invalid_state("stub: streaming not implemented"));
    }
private:
    mutable std::mutex _m;
    mutable Captured   _captured;
    Reply              _reply;
};

struct Fixture {
    std::string root;
    std::string team;
    std::string user;
    std::string chunk_a;
    std::string chunk_b;
    std::string edge_id;
    std::shared_ptr<wikore::rag::NullVectorStore>              chunk_store;
    std::shared_ptr<wikore::rag::NullRelationshipVectorStore>  edge_store;
};

Fixture seed(drogon::orm::DbClientPtr db)
{
    exec_sync(db, "DELETE FROM companies WHERE id=$1::uuid", std::string(CO));
    exec_sync(db, "INSERT INTO companies (id,name,slug) VALUES ($1::uuid,'AnsCo','ans-co')",
              std::string(CO));

    Fixture f;
    f.root = sync_scalar(db, std::format(
        "SELECT id FROM org_units WHERE company_id='{}' AND type='root'", CO));
    f.team = sync_scalar(db, std::format(
        "INSERT INTO org_units (company_id,parent_id,type,slug,name) "
        "VALUES ('{}','{}','team','ans-team','Ans Team') RETURNING id",
        CO, f.root));

    f.user = sync_scalar(db, std::format(
        "INSERT INTO users (company_id,external_sub,email) "
        "VALUES ('{}','sub-ans','ans@test') RETURNING id", CO));
    exec_sync(db, std::format(
        "INSERT INTO memberships (company_id,user_id,org_unit_id,role,applies_to) "
        "VALUES ('{}','{}','{}','viewer','self_and_descendants')",
        CO, f.user, f.team));

    const auto doc = sync_scalar(db, std::format(
        "INSERT INTO documents (company_id,owner_org_unit_id,filename,title,mime_type) "
        "VALUES ('{}','{}','a.txt','A','text/plain') RETURNING id", CO, f.team));
    const auto ver = sync_scalar(db, std::format(
        "INSERT INTO document_versions (company_id,document_id,version_no,source_hash,"
        "ingest_status,completed_at,activated_at,chunk_count,lifecycle_status,sensitivity_label) "
        "VALUES ('{}','{}',1,'h','done',now(),now(),2,'active','internal') RETURNING id",
        CO, doc));
    f.chunk_a = sync_scalar(db, std::format(
        "INSERT INTO document_chunks (company_id,document_version_id,chunk_index,"
        "content,content_hash,qdrant_prefilter_scope_ids) "
        "VALUES ('{}','{}',0,'The answer is fourty-two.','h-a','{{}}') RETURNING id",
        CO, ver));
    f.chunk_b = sync_scalar(db, std::format(
        "INSERT INTO document_chunks (company_id,document_version_id,chunk_index,"
        "content,content_hash,qdrant_prefilter_scope_ids) "
        "VALUES ('{}','{}',1,'Contradiction: it is twelve.','h-b','{{}}') RETURNING id",
        CO, ver));

    auto make_pt = [&](const std::string& chunk_id) {
        wikore::rag::ChunkPayload p;
        p.company_id          = CO;
        p.document_version_id = ver;
        p.chunk_id            = chunk_id;
        p.access_scope_ids    = {f.team};
        p.sensitivity_label   = "internal";
        p.lifecycle_status    = "active";
        return wikore::rag::UpsertPoint{
            .id = "pt-" + chunk_id,
            .vector = wikore::rag::Embedding(DIM, 0.1f),
            .payload = std::move(p)};
    };
    f.chunk_store = std::make_shared<wikore::rag::NullVectorStore>();
    drogon::sync_wait(f.chunk_store->upsert(std::vector<wikore::rag::UpsertPoint>{
        make_pt(f.chunk_a), make_pt(f.chunk_b),
    }));

    // A relationship with origin=llm_proposal so we can assert the
    // system prompt correctly warns the model about it.
    f.edge_id = sync_scalar(db, "SELECT gen_random_uuid()");
    exec_sync(db, std::format(
        "WITH new_edge AS ( "
        "  INSERT INTO knowledge_edges "
        "    (id, company_id, edge_type, direction, confidence, origin, review_state) "
        "  VALUES ('{}','{}','contradicts','symmetric',0.7,'llm_proposal','accepted') "
        "  RETURNING id, company_id "
        ") "
        "INSERT INTO knowledge_edge_endpoints "
        "  (company_id, edge_id, ordinal, chunk_id, role) "
        "SELECT company_id, id, 0, '{}'::uuid, 'a' FROM new_edge "
        "UNION ALL "
        "SELECT company_id, id, 1, '{}'::uuid, 'b' FROM new_edge",
        f.edge_id, CO, f.chunk_a, f.chunk_b));

    f.edge_store = std::make_shared<wikore::rag::NullRelationshipVectorStore>();
    f.edge_store->add(wikore::rag::EdgeCandidate{
        .edge_id             = f.edge_id,
        .company_id          = CO,
        .edge_type           = "contradicts",
        .score               = 0.95f,
        .edge_version        = 1,
        .formula_version     = 1,
        .endpoint_0_chunk_id = "",
        .endpoint_1_chunk_id = "",
    }, /*confidence=*/0.7, "accepted");

    return f;
}

std::shared_ptr<wikore::rag::AnswerUseCase>
make_use_case(drogon::orm::DbClientPtr                                  db,
              std::shared_ptr<wikore::rag::VectorStorePort>             chunk_store,
              std::shared_ptr<wikore::rag::RelationshipVectorStorePort> edge_store,
              std::shared_ptr<wikore::rag::LlmProviderPort>             llm)
{
    auto orch = std::make_shared<wikore::rag::RetrievalOrchestrator>(
        std::make_shared<wikore::rag::NullEmbedder>(DIM),
        std::make_shared<wikore::PostgresAccessResolver>(db),
        std::move(chunk_store),
        wikore::rag::EvidenceGate(db),
        std::move(edge_store),
        std::make_shared<wikore::rag::RelationshipEvidenceGate>(db));
    auto ctx_builder = std::make_shared<wikore::rag::ContextBuilder>();
    return std::make_shared<wikore::rag::AnswerUseCase>(
        std::move(orch), std::move(ctx_builder), std::move(llm));
}

wikore::RequestContext make_ctx(const std::string& user)
{
    return wikore::RequestContext{
        .tenant    = {.company_id = CO},
        .principal = {.user_id = user, .email = "ans@test"},
        .span      = {},
        .deadline  = std::chrono::steady_clock::now() + std::chrono::seconds(30),
    };
}

} // namespace

TEST_CASE("AnswerUseCase runs retrieve → build → llm and returns the completion",
          "[integration][answer]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    auto f  = seed(db);
    auto llm = std::make_shared<StubLlm>();
    llm->set_reply({.content = "42 per [SRC 1]", .input_tokens = 200,
                    .output_tokens = 5, .model = "stub-42"});
    auto uc = make_use_case(db, f.chunk_store, f.edge_store, llm);

    wikore::rag::AnswerOptions opts;
    opts.intent = wikore::rag::RetrievalIntent::Fact;
    auto r = drogon::sync_wait(uc->run(make_ctx(f.user), "what is the answer?",
                                       f.team, opts));
    REQUIRE(r);
    CHECK(r->answer == "42 per [SRC 1]");
    CHECK(r->model  == "stub-42");
    CHECK(r->input_tokens  == 200);
    CHECK(r->output_tokens == 5);
    // Fact intent: two chunks in the prompt, no edges.
    CHECK(r->source_chunk_ids.size() == 2);
    CHECK(r->source_edge_ids.empty());
    CHECK(r->evidence_included == 2);

    // The LLM saw the answer-semantics system prompt.
    const auto cap = llm->captured();
    REQUIRE(cap.messages.size() == 2);
    CHECK(cap.messages[0].role == "system");
    CHECK(cap.messages[0].content.find("origin=llm_proposal") != std::string::npos);
    CHECK(cap.messages[1].role == "user");
    // User message carries the question and the SRC blocks.
    CHECK(cap.messages[1].content.find("Question: what is the answer?")
          != std::string::npos);
    CHECK(cap.messages[1].content.find("[SRC 1]") != std::string::npos);
    // LLM call took a finite, positive per-call timeout (bounded by the
    // request deadline).
    CHECK(cap.timeout_s > 0.0);
    CHECK(cap.timeout_s <= 30.0);
}

TEST_CASE("AnswerUseCase bridge intent includes [REL N] and origin=llm_proposal",
          "[integration][answer]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    auto f  = seed(db);
    auto llm = std::make_shared<StubLlm>();
    auto uc = make_use_case(db, f.chunk_store, f.edge_store, llm);

    wikore::rag::AnswerOptions opts;
    opts.intent = wikore::rag::RetrievalIntent::Bridge;
    auto r = drogon::sync_wait(uc->run(make_ctx(f.user), "any contradictions?",
                                       f.team, opts));
    REQUIRE(r);
    CHECK(r->source_edge_ids.size() == 1);
    CHECK(r->source_edge_ids[0] == f.edge_id);

    const auto cap = llm->captured();
    // The rendered prompt carries the edge with its origin so the model
    // knows this is a model-proposed relationship (per docs
    // §"Context construction and answer semantics").
    CHECK(cap.messages[1].content.find("[REL 1: contradicts") != std::string::npos);
    CHECK(cap.messages[1].content.find("origin=llm_proposal") != std::string::npos);
    CHECK(cap.messages[1].content.find("Direction: symmetric") != std::string::npos);
}

TEST_CASE("AnswerUseCase caller-supplied system prompt overrides the default",
          "[integration][answer]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    auto f  = seed(db);
    auto llm = std::make_shared<StubLlm>();
    auto uc = make_use_case(db, f.chunk_store, f.edge_store, llm);

    wikore::rag::AnswerOptions opts;
    opts.intent = wikore::rag::RetrievalIntent::Fact;
    opts.context_opts.system_prompt = "You are a strict evaluator harness.";

    auto r = drogon::sync_wait(uc->run(make_ctx(f.user), "q", f.team, opts));
    REQUIRE(r);
    const auto cap = llm->captured();
    CHECK(cap.messages[0].content == "You are a strict evaluator harness.");
    // The default answer-semantics prompt must NOT have leaked in.
    CHECK(cap.messages[0].content.find("origin=llm_proposal") == std::string::npos);
}

TEST_CASE("AnswerUseCase propagates the LLM error kind unchanged",
          "[integration][answer]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    auto f  = seed(db);
    auto llm = std::make_shared<StubLlm>();
    llm->set_reply({.error = wikore::Error::unavailable("upstream timeout")});
    auto uc = make_use_case(db, f.chunk_store, f.edge_store, llm);

    wikore::rag::AnswerOptions opts;
    opts.intent = wikore::rag::RetrievalIntent::Fact;
    auto r = drogon::sync_wait(uc->run(make_ctx(f.user), "q", f.team, opts));
    REQUIRE_FALSE(r);
    CHECK(r.error().kind == wikore::Error::Kind::ServiceUnavailable);
}

TEST_CASE("AnswerUseCase does not call the LLM when retrieval fails",
          "[integration][answer]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    auto f  = seed(db);
    auto llm = std::make_shared<StubLlm>();
    auto uc = make_use_case(db, f.chunk_store, f.edge_store, llm);

    // Non-positive limit → retrieve_evidence returns invalid_input
    // without touching Qdrant. Assert the LLM was never invoked.
    wikore::rag::AnswerOptions opts;
    opts.intent = wikore::rag::RetrievalIntent::Fact;
    opts.limit  = 0;
    auto r = drogon::sync_wait(uc->run(make_ctx(f.user), "q", f.team, opts));
    REQUIRE_FALSE(r);
    CHECK(r.error().kind == wikore::Error::Kind::InvalidInput);
    CHECK(llm->captured().calls == 0);
}

TEST_CASE("AnswerUseCase caps the LLM per-call timeout at the request deadline",
          "[integration][answer]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    auto f  = seed(db);
    auto llm = std::make_shared<StubLlm>();
    auto uc = make_use_case(db, f.chunk_store, f.edge_store, llm);

    wikore::RequestContext ctx = make_ctx(f.user);
    // 2s from now — the LLM must see a timeout ≤ 2s regardless of the
    // caller-passed llm_timeout_s.
    ctx.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);

    wikore::rag::AnswerOptions opts;
    opts.intent        = wikore::rag::RetrievalIntent::Fact;
    opts.llm_timeout_s = 30.0;

    auto r = drogon::sync_wait(uc->run(ctx, "q", f.team, opts));
    REQUIRE(r);
    CHECK(llm->captured().timeout_s > 0.0);
    CHECK(llm->captured().timeout_s <= 2.0);
}
