#include <catch2/catch_test_macros.hpp>
#include "wikore/rag/evidence_gate.hpp"
#include "wikore/db.hpp"
#include <drogon/drogon.h>
#include <drogon/utils/coroutine.h>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

// Integration tests for EvidenceGate (the G1 access boundary). Skip without
// DATABASE_URL. The visibility LOGIC is fuzzed by the section-5 property test;
// here we test the gate wrapper: it filters candidates to the allowed subset,
// hydrates text/section_heading, carries score/order, denies on
// lifecycle/sensitivity, enforces principal_applies_to, and fails closed.

namespace {

constexpr auto CO = "61a7e000-0000-0000-0000-0000000000a1"; // "gate co"

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

std::string g_root;

void seed(drogon::orm::DbClientPtr db)
{
    exec_sync(db, "DELETE FROM companies WHERE id=$1::uuid", std::string(CO));
    exec_sync(db, "INSERT INTO companies (id,name,slug) VALUES ($1::uuid,'Gate','gate')",
              std::string(CO));
    g_root = std::string(exec_sync(db,
        "SELECT id FROM org_units WHERE company_id=$1::uuid AND type='root'",
        std::string(CO))[0]["id"].c_str());
}

std::string make_ou(drogon::orm::DbClientPtr db, const std::string& parent, const std::string& slug)
{
    return std::string(exec_sync(db,
        "INSERT INTO org_units (company_id,parent_id,type,slug,name) "
        "VALUES ($1::uuid,$2::uuid,'team',$3,$3) RETURNING id",
        std::string(CO), parent, slug)[0]["id"].c_str());
}

struct ChunkRef { std::string chunk_id, version_id; };

ChunkRef make_chunk(drogon::orm::DbClientPtr db, const std::string& owner,
                    const std::string& lifecycle = "active",
                    const std::string& sensitivity = "internal",
                    std::optional<std::string> heading = std::nullopt)
{
    static int n = 0; ++n;
    const auto doc = std::string(exec_sync(db,
        "INSERT INTO documents (company_id,owner_org_unit_id,filename) "
        "VALUES ($1::uuid,$2::uuid,$3) RETURNING id",
        std::string(CO), owner, "d" + std::to_string(n) + ".txt")[0]["id"].c_str());

    std::string ver;
    if (lifecycle == "active") {
        ver = std::string(exec_sync(db,
            "INSERT INTO document_versions (company_id,document_id,version_no,source_hash,"
            "ingest_status,completed_at,activated_at,chunk_count,lifecycle_status,sensitivity_label) "
            "VALUES ($1::uuid,$2::uuid,1,'h','done',now(),now(),1,'active',$3) RETURNING id",
            std::string(CO), doc, sensitivity)[0]["id"].c_str());
    } else {
        ver = std::string(exec_sync(db,
            "INSERT INTO document_versions (company_id,document_id,version_no,source_hash,"
            "lifecycle_status,sensitivity_label) "
            "VALUES ($1::uuid,$2::uuid,1,'h',$3,$4) RETURNING id",
            std::string(CO), doc, lifecycle, sensitivity)[0]["id"].c_str());
    }

    std::optional<std::string> sec_id;
    if (heading) {
        sec_id = std::string(exec_sync(db,
            "INSERT INTO document_sections (company_id,document_version_id,ordinal,depth,heading,heading_path) "
            "VALUES ($1::uuid,$2::uuid,0,0,$3,'{}') RETURNING id",
            std::string(CO), ver, *heading)[0]["id"].c_str());
    }
    const auto chunk = std::string(exec_sync(db,
        "INSERT INTO document_chunks (company_id,document_version_id,chunk_index,content,"
        "content_hash,qdrant_prefilter_scope_ids,section_id) "
        "VALUES ($1::uuid,$2::uuid,0,$3,$4,'{}',$5::uuid) RETURNING id",
        std::string(CO), ver, "body of " + doc, "h" + doc, sec_id)[0]["id"].c_str());
    return {chunk, ver};
}

wikore::rag::ChunkCandidate cand(const ChunkRef& r, float score)
{
    return wikore::rag::ChunkCandidate{
        .chunk_id = r.chunk_id, .document_version_id = r.version_id,
        .score = score, .payload = {}};
}

const std::vector<std::string> kMember{"public", "internal", "confidential"};

} // namespace

TEST_CASE("EvidenceGate: returns only visible candidates, hydrated, in score order",
          "[integration][gate]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get(); seed(db);
    const auto A = make_ou(db, g_root, "a");
    const auto B = make_ou(db, g_root, "b");

    auto visible = make_chunk(db, A, "active", "internal", "Intro");  // owner A
    auto denied  = make_chunk(db, B);                                  // owner B, no grant

    wikore::AccessScope scope; scope.org_unit_ids = {A};   // reader is in A
    wikore::rag::EvidenceGate gate(db);

    auto r = drogon::sync_wait(gate.evaluate(
        CO, scope, kMember, {cand(visible, 0.9f), cand(denied, 0.8f)}));
    REQUIRE(r.has_value());
    REQUIRE(r->size() == 1);
    CHECK((*r)[0].chunk_id() == visible.chunk_id);
    CHECK((*r)[0].score() == 0.9f);                           // score carried
    CHECK((*r)[0].text().find("body of") != std::string::npos); // hydrated from PG
    REQUIRE((*r)[0].section_heading().has_value());
    CHECK(*(*r)[0].section_heading() == "Intro");             // section hydrated
}

TEST_CASE("EvidenceGate: denies a chunk on a non-active version (lifecycle)",
          "[integration][gate]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get(); seed(db);
    const auto A = make_ou(db, g_root, "a");
    auto draft = make_chunk(db, A, "draft");                // owner A but not active

    wikore::AccessScope scope; scope.org_unit_ids = {A};
    wikore::rag::EvidenceGate gate(db);
    auto r = drogon::sync_wait(gate.evaluate(CO, scope, kMember, {cand(draft, 0.9f)}));
    REQUIRE(r.has_value());
    CHECK(r->empty());
}

TEST_CASE("EvidenceGate: denies above-clearance sensitivity, admits when cleared",
          "[integration][gate]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get(); seed(db);
    const auto A = make_ou(db, g_root, "a");
    auto restricted = make_chunk(db, A, "active", "restricted");

    wikore::AccessScope scope; scope.org_unit_ids = {A};
    wikore::rag::EvidenceGate gate(db);

    auto deny = drogon::sync_wait(gate.evaluate(CO, scope, kMember, {cand(restricted, 0.9f)}));
    REQUIRE(deny.has_value());
    CHECK(deny->empty());                                    // member cannot see restricted

    auto allow = drogon::sync_wait(gate.evaluate(
        CO, scope, {"public","internal","confidential","restricted"}, {cand(restricted, 0.9f)}));
    REQUIRE(allow.has_value());
    CHECK(allow->size() == 1);
}

TEST_CASE("EvidenceGate: self_only-principal grant does not leak to a descendant reader",
          "[integration][gate]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get(); seed(db);
    const auto A = make_ou(db, g_root, "a");
    const auto B = make_ou(db, A, "b");                      // B is a child of A
    auto chunk = make_chunk(db, A);                          // doc owned by A

    // Grant: "A only" (self_only principal) may read the A subtree.
    exec_sync(db,
        "INSERT INTO resource_grants (company_id,resource_type,resource_id,resource_applies_to,"
        "principal_type,principal_id,principal_applies_to,permission) "
        "VALUES ($1::uuid,'org_unit',$2::uuid,'self_and_descendants','org_unit',$2::uuid,'self_only','read')",
        std::string(CO), A);

    wikore::rag::EvidenceGate gate(db);

    // Reader in B (descendant of A) must NOT see it: principal is self_only=A.
    wikore::AccessScope inB; inB.org_unit_ids = {B};
    auto deny = drogon::sync_wait(gate.evaluate(CO, inB, kMember, {cand(chunk, 0.9f)}));
    REQUIRE(deny.has_value());
    CHECK(deny->empty());

    // Reader in A is the grant's principal -> allowed (positive control).
    wikore::AccessScope inA; inA.org_unit_ids = {A};
    auto allow = drogon::sync_wait(gate.evaluate(CO, inA, kMember, {cand(chunk, 0.9f)}));
    REQUIRE(allow.has_value());
    CHECK(allow->size() == 1);
}

TEST_CASE("EvidenceGate: document_version_id is taken from Postgres, not the candidate (P1)",
          "[integration][gate]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get(); seed(db);
    const auto A = make_ou(db, g_root, "a");
    auto ref = make_chunk(db, A);                  // authoritative version = ref.version_id

    // Candidate carries a bogus version, as a stale Qdrant payload might.
    wikore::rag::ChunkCandidate bogus{
        .chunk_id = ref.chunk_id,
        .document_version_id = "00000000-0000-0000-0000-000000000000",
        .score = 0.9f, .payload = {}};

    wikore::AccessScope scope; scope.org_unit_ids = {A};
    wikore::rag::EvidenceGate gate(db);
    auto r = drogon::sync_wait(gate.evaluate(CO, scope, kMember, {bogus}));
    REQUIRE(r.has_value());
    REQUIRE(r->size() == 1);
    CHECK((*r)[0].document_version_id() == ref.version_id);   // PG value, not the bogus one
}

TEST_CASE("EvidenceGate: fail-closed on empty scope, empty clearance, or no candidates",
          "[integration][gate]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get(); seed(db);
    const auto A = make_ou(db, g_root, "a");
    auto chunk = make_chunk(db, A);
    wikore::rag::EvidenceGate gate(db);

    wikore::AccessScope empty_scope;
    wikore::AccessScope full; full.org_unit_ids = {A};

    auto a = drogon::sync_wait(gate.evaluate(CO, empty_scope, kMember, {cand(chunk, 1.f)}));
    REQUIRE(a.has_value()); CHECK(a->empty());

    auto b = drogon::sync_wait(gate.evaluate(CO, full, /*labels=*/{}, {cand(chunk, 1.f)}));
    REQUIRE(b.has_value()); CHECK(b->empty());

    auto c = drogon::sync_wait(gate.evaluate(CO, full, kMember, /*candidates=*/{}));
    REQUIRE(c.has_value()); CHECK(c->empty());
}
