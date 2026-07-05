#include <catch2/catch_test_macros.hpp>
#include "wikore/rag/relationship_evidence_gate.hpp"
#include "wikore/adapters/postgres/deadline_exec.hpp"
#include "wikore/db.hpp"
#include <drogon/drogon.h>
#include <drogon/utils/coroutine.h>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Integration tests for RelationshipEvidenceGate — the G2 authorization
// boundary that admits an edge only when BOTH endpoints pass live
// authorization (per docs §"No partial relationship hydration").
//
// Uses a fresh tenant per file so seed state is isolated from other suites.
// Skips without DATABASE_URL. Chunk-side visibility logic is reused from G1
// (same three arms); this file tests the WHOLE-RELATIONSHIP invariant, the
// review_state / lifecycle filters, tenant scoping, and endpoint hydration.
// ---------------------------------------------------------------------------

namespace {

constexpr auto CO_REL = "16e10000-0000-0000-0000-0000000000c1";  // "rel co"

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

struct Seed {
    std::string root;
    std::string team_a;
    std::string team_b;
    std::string doc_a;
    std::string doc_b;
    std::string ver_a;
    std::string ver_b;
    std::string chunk_a;    // owned by team_a
    std::string chunk_b;    // owned by team_b
    std::string chunk_c;    // owned by team_a
};

std::string sync_scalar(drogon::orm::DbClientPtr db, std::string sql)
{
    auto rows = exec_sync(db, std::move(sql));
    return std::string(rows[0][0].c_str());
}

Seed seed(drogon::orm::DbClientPtr db)
{
    exec_sync(db, "DELETE FROM companies WHERE id=$1::uuid", std::string(CO_REL));
    exec_sync(db, "INSERT INTO companies (id,name,slug) VALUES ($1::uuid,'Rel','rel-gate')",
              std::string(CO_REL));

    Seed s;
    s.root   = sync_scalar(db, std::format(
        "SELECT id FROM org_units WHERE company_id='{}' AND type='root'", CO_REL));
    s.team_a = sync_scalar(db, std::format(
        "INSERT INTO org_units (company_id,parent_id,type,slug,name) "
        "VALUES ('{}','{}','team','ta','Team A') RETURNING id", CO_REL, s.root));
    s.team_b = sync_scalar(db, std::format(
        "INSERT INTO org_units (company_id,parent_id,type,slug,name) "
        "VALUES ('{}','{}','team','tb','Team B') RETURNING id", CO_REL, s.root));

    // Two documents, one owned by team_a, one by team_b.
    s.doc_a = sync_scalar(db, std::format(
        "INSERT INTO documents (company_id,owner_org_unit_id,filename,title,mime_type) "
        "VALUES ('{}','{}','a.txt','A','text/plain') RETURNING id", CO_REL, s.team_a));
    s.doc_b = sync_scalar(db, std::format(
        "INSERT INTO documents (company_id,owner_org_unit_id,filename,title,mime_type) "
        "VALUES ('{}','{}','b.txt','B','text/plain') RETURNING id", CO_REL, s.team_b));

    s.ver_a = sync_scalar(db, std::format(
        "INSERT INTO document_versions (company_id,document_id,version_no,source_hash,"
        "ingest_status,completed_at,activated_at,chunk_count,lifecycle_status,sensitivity_label) "
        "VALUES ('{}','{}',1,'ha','done',now(),now(),2,'active','internal') RETURNING id",
        CO_REL, s.doc_a));
    s.ver_b = sync_scalar(db, std::format(
        "INSERT INTO document_versions (company_id,document_id,version_no,source_hash,"
        "ingest_status,completed_at,activated_at,chunk_count,lifecycle_status,sensitivity_label) "
        "VALUES ('{}','{}',1,'hb','done',now(),now(),1,'active','internal') RETURNING id",
        CO_REL, s.doc_b));

    s.chunk_a = sync_scalar(db, std::format(
        "INSERT INTO document_chunks (company_id,document_version_id,chunk_index,"
        "content,content_hash) "
        "VALUES ('{}','{}',0,'chunk-A-text','hca') RETURNING id", CO_REL, s.ver_a));
    s.chunk_b = sync_scalar(db, std::format(
        "INSERT INTO document_chunks (company_id,document_version_id,chunk_index,"
        "content,content_hash) "
        "VALUES ('{}','{}',0,'chunk-B-text','hcb') RETURNING id", CO_REL, s.ver_b));
    s.chunk_c = sync_scalar(db, std::format(
        "INSERT INTO document_chunks (company_id,document_version_id,chunk_index,"
        "content,content_hash) "
        "VALUES ('{}','{}',1,'chunk-C-text','hcc') RETURNING id", CO_REL, s.ver_a));

    return s;
}

// Insert a knowledge_edges + endpoints pair via the same CTE the repo
// uses (single statement so the deferred CONSTRAINT TRIGGER fires at
// statement commit).
//
// When seeding a past expires_at (used to test expired-edge redaction),
// created_at MUST also be stamped in the past — V034's CHECK enforces
// expires_at IS NULL OR expires_at > created_at, and created_at
// defaults to now(). Callers pass a matching created_at.
std::string
make_edge(drogon::orm::DbClientPtr db,
          const std::string& chunk0, const std::string& chunk1,
          const std::string& review_state = "accepted",
          std::optional<std::string> created_at = std::nullopt,
          std::optional<std::string> expires_at = std::nullopt,
          std::optional<std::string> superseded_at = std::nullopt)
{
    auto id = sync_scalar(db, "SELECT gen_random_uuid()");
    const std::string cre_frag = created_at
        ? std::format(",'{}'::timestamptz", *created_at) : ",DEFAULT";
    const std::string exp_frag = expires_at
        ? std::format(",'{}'::timestamptz", *expires_at) : ",NULL";
    const std::string sup_frag = superseded_at
        ? std::format(",'{}'::timestamptz", *superseded_at) : ",NULL";
    exec_sync(db, std::format(
        "WITH new_edge AS ( "
        "  INSERT INTO knowledge_edges "
        "    (id, company_id, edge_type, direction, confidence, origin, "
        "     review_state, created_at, expires_at, superseded_at) "
        "  VALUES ('{}','{}','implements','directed',0.9,'administrator', "
        "          '{}' {} {} {}) "
        "  RETURNING id, company_id "
        ") "
        "INSERT INTO knowledge_edge_endpoints "
        "  (company_id, edge_id, ordinal, chunk_id, role) "
        "SELECT company_id, id, 0, '{}'::uuid, 'source' FROM new_edge "
        "UNION ALL "
        "SELECT company_id, id, 1, '{}'::uuid, 'target' FROM new_edge",
        id, CO_REL, review_state, cre_frag, exp_frag, sup_frag, chunk0, chunk1));
    return id;
}

wikore::rag::EdgeCandidate
cand(const std::string& edge_id, float score = 0.9f)
{
    return wikore::rag::EdgeCandidate{
        .edge_id             = edge_id,
        .company_id          = CO_REL,
        .edge_type           = "implements",
        .score               = score,
        .edge_version        = 1,
        .formula_version     = 1,
        .endpoint_0_chunk_id = "",  // advisory; gate ignores
        .endpoint_1_chunk_id = "",
    };
}

wikore::AccessScope scope_of(const std::string& ou_id)
{
    return wikore::AccessScope{.org_unit_ids = {ou_id}};
}

std::vector<std::string> INTERNAL = {"internal"};
std::vector<std::string> ACCEPTED = {"accepted"};
// ACTIVE not used — the gate call sites pass their lifecycle inline
// so the intent is visible at the site rather than hidden in a helper.

} // namespace

TEST_CASE("RelationshipEvidenceGate: both endpoints visible → edge admitted with hydrated text",
          "[integration][relationship_gate]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    auto s = seed(db);
    // Two team_a chunks — a scope holding team_a admits both.
    auto edge_id = make_edge(db, s.chunk_a, s.chunk_c);

    wikore::rag::RelationshipEvidenceGate gate(db);
    auto result = drogon::sync_wait(gate.evaluate(
        CO_REL, scope_of(s.team_a), INTERNAL, ACCEPTED,
        {cand(edge_id)},
        std::vector<std::string>{"active"}, wikore::postgres::no_deadline()));
    REQUIRE(result.has_value());
    REQUIRE(result->size() == 1);
    const auto& r = (*result)[0];
    CHECK(r.edge_id() == edge_id);
    CHECK(r.company_id() == CO_REL);
    CHECK(r.edge_type() == "implements");
    CHECK(r.direction() == "directed");
    CHECK(r.review_state() == "accepted");
    // Hydration — text and section come from PG, not from the advisory
    // payload the candidate carried.
    CHECK(r.endpoint_0().chunk_id == s.chunk_a);
    CHECK(r.endpoint_0().text == "chunk-A-text");
    CHECK(r.endpoint_1().chunk_id == s.chunk_c);
    CHECK(r.endpoint_1().text == "chunk-C-text");
    // Score carried from candidate.
    CHECK(r.score() == 0.9f);
}

TEST_CASE("RelationshipEvidenceGate: one endpoint invisible → whole edge redacted",
          "[integration][relationship_gate]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    auto s = seed(db);
    // Cross-team edge: team_a chunk + team_b chunk. A scope holding
    // ONLY team_a should redact the entire edge — existence must not
    // leak (docs §"No partial relationship hydration").
    auto edge_id = make_edge(db, s.chunk_a, s.chunk_b);

    wikore::rag::RelationshipEvidenceGate gate(db);
    auto result = drogon::sync_wait(gate.evaluate(
        CO_REL, scope_of(s.team_a), INTERNAL, ACCEPTED, {cand(edge_id)},
        std::vector<std::string>{"active"}, wikore::postgres::no_deadline()));
    REQUIRE(result.has_value());
    // Whole-relationship invariant: NOT partially hydrated, entirely dropped.
    CHECK(result->empty());
}

TEST_CASE("RelationshipEvidenceGate: expanded scope (both teams) admits the cross-team edge",
          "[integration][relationship_gate]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    auto s = seed(db);
    auto edge_id = make_edge(db, s.chunk_a, s.chunk_b);

    wikore::rag::RelationshipEvidenceGate gate(db);
    wikore::AccessScope scope{.org_unit_ids = {s.team_a, s.team_b}};
    auto result = drogon::sync_wait(gate.evaluate(
        CO_REL, scope, INTERNAL, ACCEPTED, {cand(edge_id)},
        std::vector<std::string>{"active"}, wikore::postgres::no_deadline()));
    REQUIRE(result.has_value());
    REQUIRE(result->size() == 1);
    CHECK((*result)[0].edge_id() == edge_id);
}

TEST_CASE("RelationshipEvidenceGate: review_state whitelist excludes non-accepted edges",
          "[integration][relationship_gate]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    auto s = seed(db);
    auto accepted_id = make_edge(db, s.chunk_a, s.chunk_c, "accepted");
    auto proposed_id = make_edge(db, s.chunk_a, s.chunk_c, "proposed");

    wikore::rag::RelationshipEvidenceGate gate(db);
    auto result = drogon::sync_wait(gate.evaluate(
        CO_REL, scope_of(s.team_a), INTERNAL, ACCEPTED,
        {cand(accepted_id), cand(proposed_id)},
        std::vector<std::string>{"active"}, wikore::postgres::no_deadline()));
    REQUIRE(result.has_value());
    REQUIRE(result->size() == 1);
    CHECK((*result)[0].edge_id() == accepted_id);
}

TEST_CASE("RelationshipEvidenceGate: expired edge is dropped",
          "[integration][relationship_gate]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    auto s = seed(db);
    auto expired = make_edge(db, s.chunk_a, s.chunk_c, "accepted",
                             /*created_at=*/std::string("1999-01-01T00:00:00Z"),
                             /*expires_at=*/std::string("2000-01-01T00:00:00Z"));

    wikore::rag::RelationshipEvidenceGate gate(db);
    auto result = drogon::sync_wait(gate.evaluate(
        CO_REL, scope_of(s.team_a), INTERNAL, ACCEPTED, {cand(expired)},
        std::vector<std::string>{"active"}, wikore::postgres::no_deadline()));
    REQUIRE(result.has_value());
    CHECK(result->empty());
}

TEST_CASE("RelationshipEvidenceGate: cross-tenant company_id returns nothing",
          "[integration][relationship_gate]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    auto s = seed(db);
    auto edge_id = make_edge(db, s.chunk_a, s.chunk_c);

    wikore::rag::RelationshipEvidenceGate gate(db);
    // Query with a made-up other company_id — the gate must not admit
    // an edge that belongs to CO_REL.
    auto result = drogon::sync_wait(gate.evaluate(
        "ffffffff-ffff-ffff-ffff-fffffffffff1",
        scope_of(s.team_a), INTERNAL, ACCEPTED, {cand(edge_id)},
        std::vector<std::string>{"active"}, wikore::postgres::no_deadline()));
    REQUIRE(result.has_value());
    CHECK(result->empty());
}

TEST_CASE("RelationshipEvidenceGate: empty inputs are fail-closed short-circuits",
          "[integration][relationship_gate]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    auto s = seed(db);
    auto edge_id = make_edge(db, s.chunk_a, s.chunk_c);

    wikore::rag::RelationshipEvidenceGate gate(db);
    // Empty scope.
    {
        auto r = drogon::sync_wait(gate.evaluate(
            CO_REL, wikore::AccessScope{}, INTERNAL, ACCEPTED, {cand(edge_id)},
        std::vector<std::string>{"active"}, wikore::postgres::no_deadline()));
        REQUIRE(r.has_value());
        CHECK(r->empty());
    }
    // Empty clearance.
    {
        auto r = drogon::sync_wait(gate.evaluate(
            CO_REL, scope_of(s.team_a), {}, ACCEPTED, {cand(edge_id)},
        std::vector<std::string>{"active"}, wikore::postgres::no_deadline()));
        REQUIRE(r.has_value());
        CHECK(r->empty());
    }
    // Empty review-state whitelist.
    {
        auto r = drogon::sync_wait(gate.evaluate(
            CO_REL, scope_of(s.team_a), INTERNAL, {}, {cand(edge_id)},
        std::vector<std::string>{"active"}, wikore::postgres::no_deadline()));
        REQUIRE(r.has_value());
        CHECK(r->empty());
    }
    // Empty candidate list.
    {
        auto r = drogon::sync_wait(gate.evaluate(
            CO_REL, scope_of(s.team_a), INTERNAL, ACCEPTED, {},
        std::vector<std::string>{"active"}, wikore::postgres::no_deadline()));
        REQUIRE(r.has_value());
        CHECK(r->empty());
    }
}

TEST_CASE("RelationshipEvidenceGate: score order is preserved across candidates",
          "[integration][relationship_gate]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    auto s = seed(db);
    auto e1 = make_edge(db, s.chunk_a, s.chunk_c);
    auto e2 = make_edge(db, s.chunk_a, s.chunk_c);
    auto e3 = make_edge(db, s.chunk_a, s.chunk_c);

    wikore::rag::RelationshipEvidenceGate gate(db);
    // Candidates arrive in Qdrant score order — gate must emit in the
    // same order so the reranker downstream sees a stable stream.
    std::vector<wikore::rag::EdgeCandidate> cands = {
        cand(e2, 0.95f), cand(e1, 0.80f), cand(e3, 0.60f),
    };
    auto result = drogon::sync_wait(gate.evaluate(
        CO_REL, scope_of(s.team_a), INTERNAL, ACCEPTED, cands,
        std::vector<std::string>{"active"}, wikore::postgres::no_deadline()));
    REQUIRE(result.has_value());
    REQUIRE(result->size() == 3);
    CHECK((*result)[0].edge_id() == e2);
    CHECK((*result)[1].edge_id() == e1);
    CHECK((*result)[2].edge_id() == e3);
}
