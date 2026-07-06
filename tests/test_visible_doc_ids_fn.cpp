#include <catch2/catch_test_macros.hpp>
#include "wikore/rag/evidence_gate.hpp"
#include "wikore/rag/relationship_evidence_gate.hpp"
#include "wikore/adapters/postgres/deadline_exec.hpp"
#include "wikore/db.hpp"

#include <drogon/drogon.h>
#include <drogon/utils/coroutine.h>

#include <cstdlib>
#include <format>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// V038 parity tests. The three visibility arms (ownership, direct-document
// grant, org-unit grant with closure) now live in the shared
// wikore_visible_doc_ids Postgres function; both EvidenceGate (G1) and
// RelationshipEvidenceGate (G2) call it.
//
// These tests seed access via each of the three arms in turn and assert
// that G1 and G2 return IDENTICAL doc-visibility outcomes for the same
// underlying grant. A regression here (one gate admits, the other
// redacts) would be a security-drift bug — historically the arms were
// duplicated inline and the review flagged this class of drift as the
// standing security risk closed by V038.
//
// Skips without DATABASE_URL.
// ---------------------------------------------------------------------------

namespace {

constexpr auto CO = "17038000-0000-0000-0000-000000000001"; // "V038"

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
    return std::string(exec_sync(db, std::move(sql))[0][0].c_str());
}

// Seed: two teams (A, B). A reader who is a viewer of team_B (only).
// Doc lives under team_A. Chunks: c_a1 in the doc; also a second chunk
// c_a2 so we can make an edge between them (both in the same doc — the
// visibility test is per doc, not per chunk).
struct Fixture {
    std::string root;
    std::string team_a;
    std::string team_b;
    std::string reader;
    std::string doc_a;      // owned by team_a
    std::string chunk_1;    // in doc_a
    std::string chunk_2;    // in doc_a
    std::string edge_id;    // relationship over (chunk_1, chunk_2)
};

Fixture seed(drogon::orm::DbClientPtr db)
{
    exec_sync(db, "DELETE FROM companies WHERE id=$1::uuid", std::string(CO));
    exec_sync(db, "INSERT INTO companies (id,name,slug) VALUES ($1::uuid,'V038','v038')",
              std::string(CO));

    Fixture f;
    f.root = sync_scalar(db, std::format(
        "SELECT id FROM org_units WHERE company_id='{}' AND type='root'", CO));
    f.team_a = sync_scalar(db, std::format(
        "INSERT INTO org_units (company_id,parent_id,type,slug,name) "
        "VALUES ('{}','{}','team','ta','A') RETURNING id", CO, f.root));
    f.team_b = sync_scalar(db, std::format(
        "INSERT INTO org_units (company_id,parent_id,type,slug,name) "
        "VALUES ('{}','{}','team','tb','B') RETURNING id", CO, f.root));

    f.reader = sync_scalar(db, std::format(
        "INSERT INTO users (company_id,external_sub,email) "
        "VALUES ('{}','v038-sub','v038@test') RETURNING id", CO));
    // Reader is only in team_B — must NOT see doc_a via ownership.
    exec_sync(db, std::format(
        "INSERT INTO memberships (company_id,user_id,org_unit_id,role,applies_to) "
        "VALUES ('{}','{}','{}','viewer','self_and_descendants')",
        CO, f.reader, f.team_b));

    f.doc_a = sync_scalar(db, std::format(
        "INSERT INTO documents (company_id,owner_org_unit_id,filename,title,mime_type) "
        "VALUES ('{}','{}','a.txt','A','text/plain') RETURNING id", CO, f.team_a));
    const auto ver = sync_scalar(db, std::format(
        "INSERT INTO document_versions (company_id,document_id,version_no,source_hash,"
        "ingest_status,completed_at,activated_at,chunk_count,lifecycle_status,sensitivity_label) "
        "VALUES ('{}','{}',1,'h','done',now(),now(),2,'active','internal') RETURNING id",
        CO, f.doc_a));
    f.chunk_1 = sync_scalar(db, std::format(
        "INSERT INTO document_chunks (company_id,document_version_id,chunk_index,"
        "content,content_hash) "
        "VALUES ('{}','{}',0,'c1','h1') RETURNING id", CO, ver));
    f.chunk_2 = sync_scalar(db, std::format(
        "INSERT INTO document_chunks (company_id,document_version_id,chunk_index,"
        "content,content_hash) "
        "VALUES ('{}','{}',1,'c2','h2') RETURNING id", CO, ver));

    // Edge over both chunks (same doc, both endpoints admit-or-redact
    // together — perfect for the parity assertion).
    f.edge_id = sync_scalar(db, "SELECT gen_random_uuid()");
    exec_sync(db, std::format(
        "WITH new_edge AS ( "
        "  INSERT INTO knowledge_edges "
        "    (id, company_id, edge_type, direction, confidence, origin, review_state) "
        "  VALUES ('{}','{}','implements','directed',0.9,'administrator','accepted') "
        "  RETURNING id, company_id "
        ") "
        "INSERT INTO knowledge_edge_endpoints "
        "  (company_id, edge_id, ordinal, chunk_id, role) "
        "SELECT company_id, id, 0, '{}'::uuid, 'source' FROM new_edge "
        "UNION ALL "
        "SELECT company_id, id, 1, '{}'::uuid, 'target' FROM new_edge",
        f.edge_id, CO, f.chunk_1, f.chunk_2));

    return f;
}

wikore::AccessScope reader_scope(const Fixture& f)
{
    // The reader is a viewer of team_B, so scope = {team_B}.
    return wikore::AccessScope{.org_unit_ids = {f.team_b}};
}

wikore::rag::EdgeCandidate edge_cand(const std::string& edge_id)
{
    return wikore::rag::EdgeCandidate{
        .edge_id             = edge_id,
        .company_id          = CO,
        .edge_type           = "implements",
        .score               = 0.9f,
        .edge_version        = 1,
        .formula_version     = 1,
        .endpoint_0_chunk_id = "",
        .endpoint_1_chunk_id = "",
    };
}

std::vector<std::string> INTERNAL = {"internal"};
std::vector<std::string> ACCEPTED = {"accepted"};

// Run both gates against the fixture and report a bool pair (chunk_visible,
// edge_admitted). Chunk_1 is the sole candidate to G1; the edge over
// (chunk_1, chunk_2) is the candidate to G2. Since both endpoints are in
// the same doc, they admit or redact together — so G1 and G2 must agree.
struct Parity {
    bool chunk_visible = false;
    bool edge_admitted = false;
};

Parity gates(drogon::orm::DbClientPtr db, const Fixture& f)
{
    wikore::rag::EvidenceGate g1(db);
    auto r1 = drogon::sync_wait(g1.evaluate(
        CO, reader_scope(f), INTERNAL,
        std::vector<wikore::rag::ChunkCandidate>{
            {.chunk_id = f.chunk_1, .score = 0.9f}},
        std::vector<std::string>{"active"},
        wikore::postgres::no_deadline()));

    wikore::rag::RelationshipEvidenceGate g2(db);
    auto r2 = drogon::sync_wait(g2.evaluate(
        CO, reader_scope(f), INTERNAL, ACCEPTED,
        std::vector<wikore::rag::EdgeCandidate>{edge_cand(f.edge_id)},
        std::vector<std::string>{"active"},
        wikore::postgres::no_deadline()));

    REQUIRE(r1);
    REQUIRE(r2);
    return Parity{.chunk_visible = !r1->empty(),
                  .edge_admitted = !r2->empty()};
}

} // namespace

TEST_CASE("V038: G1 and G2 both redact when no arm applies (baseline)",
          "[integration][v038]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    auto f  = seed(db);
    // No grant issued. Reader is only in team_B; doc_a is owned by
    // team_A. Every arm should fail.
    const auto p = gates(db, f);
    CHECK_FALSE(p.chunk_visible);
    CHECK_FALSE(p.edge_admitted);
}

TEST_CASE("V038: arm 1 (ownership) admits identically in G1 and G2",
          "[integration][v038]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    auto f  = seed(db);
    // Move the reader to team_A so ownership admits.
    exec_sync(db, std::format(
        "UPDATE memberships SET org_unit_id='{}' "
        "WHERE user_id='{}'", f.team_a, f.reader));

    wikore::AccessScope scope{.org_unit_ids = {f.team_a}};
    wikore::rag::EvidenceGate g1(db);
    auto r1 = drogon::sync_wait(g1.evaluate(
        CO, scope, INTERNAL,
        std::vector<wikore::rag::ChunkCandidate>{
            {.chunk_id = f.chunk_1, .score = 0.9f}},
        std::vector<std::string>{"active"},
        wikore::postgres::no_deadline()));
    wikore::rag::RelationshipEvidenceGate g2(db);
    auto r2 = drogon::sync_wait(g2.evaluate(
        CO, scope, INTERNAL, ACCEPTED,
        std::vector<wikore::rag::EdgeCandidate>{edge_cand(f.edge_id)},
        std::vector<std::string>{"active"},
        wikore::postgres::no_deadline()));
    REQUIRE(r1);
    REQUIRE(r2);
    CHECK(r1->size() == 1);
    CHECK(r2->size() == 1);
}

TEST_CASE("V038: arm 2 (direct document grant) admits identically in G1 and G2",
          "[integration][v038]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    auto f  = seed(db);
    // Grant team_B (self_only) READ on doc_a. Arm 2 covers this.
    exec_sync(db, std::format(
        "INSERT INTO resource_grants "
        "  (company_id, resource_type, resource_id, resource_applies_to, "
        "   principal_type, principal_id, principal_applies_to, permission) "
        "VALUES ('{}','document','{}','self_only',"
        "        'org_unit','{}','self_only','read')",
        CO, f.doc_a, f.team_b));

    const auto p = gates(db, f);
    CHECK(p.chunk_visible);
    CHECK(p.edge_admitted);
}

TEST_CASE("V038: arm 3 (org-unit grant with closure) admits identically in G1 and G2",
          "[integration][v038]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    auto f  = seed(db);
    // Grant team_B READ on org_unit team_A (self_only, principal
    // self_only). Arm 3 covers this: closure descendant_id=team_A
    // (owner) → ancestor_id=team_A; grant resource_id=team_A at
    // depth=0 with self_only resource applies_to.
    exec_sync(db, std::format(
        "INSERT INTO resource_grants "
        "  (company_id, resource_type, resource_id, resource_applies_to, "
        "   principal_type, principal_id, principal_applies_to, permission) "
        "VALUES ('{}','org_unit','{}','self_only',"
        "        'org_unit','{}','self_only','read')",
        CO, f.team_a, f.team_b));

    const auto p = gates(db, f);
    CHECK(p.chunk_visible);
    CHECK(p.edge_admitted);
}

TEST_CASE("V038: expired arm-2 grant redacts identically in G1 and G2",
          "[integration][v038]")
{
    // Every expiry-driven change must apply to BOTH gates. Historical
    // regression risk: if only one gate honoured expires_at, a
    // no-longer-authorized reader could still see edges over the doc.
    // Grant is seeded with granted_at + expires_at both in the past to
    // satisfy the CHECK (expires_at > granted_at) constraint while
    // still being expired at query time.
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    auto f  = seed(db);
    exec_sync(db, std::format(
        "INSERT INTO resource_grants "
        "  (company_id, resource_type, resource_id, resource_applies_to, "
        "   principal_type, principal_id, principal_applies_to, "
        "   permission, granted_at, expires_at) "
        "VALUES ('{}','document','{}','self_only',"
        "        'org_unit','{}','self_only','read',"
        "        '1998-01-01T00:00:00Z','1999-01-01T00:00:00Z')",
        CO, f.doc_a, f.team_b));

    const auto p = gates(db, f);
    CHECK_FALSE(p.chunk_visible);
    CHECK_FALSE(p.edge_admitted);
}

TEST_CASE("V038: grant to a different principal is invisible in G1 and G2",
          "[integration][v038]")
{
    // A grant that gives some OTHER team access to doc_a must not
    // leak to the reader (who is in team_b). Both gates must ignore
    // grants whose principal_id is not in the reader's scope /
    // reader_grant_keys — a regression here in one gate would be a
    // cross-scope leak.
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    auto f  = seed(db);
    const auto team_c = sync_scalar(db, std::format(
        "INSERT INTO org_units (company_id,parent_id,type,slug,name) "
        "VALUES ('{}','{}','team','tc','C') RETURNING id", CO, f.root));
    exec_sync(db, std::format(
        "INSERT INTO resource_grants "
        "  (company_id, resource_type, resource_id, resource_applies_to, "
        "   principal_type, principal_id, principal_applies_to, permission) "
        "VALUES ('{}','document','{}','self_only',"
        "        'org_unit','{}','self_only','read')",
        CO, f.doc_a, team_c));

    const auto p = gates(db, f);
    CHECK_FALSE(p.chunk_visible);
    CHECK_FALSE(p.edge_admitted);
}
