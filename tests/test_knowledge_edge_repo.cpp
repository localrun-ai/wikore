#include <catch2/catch_test_macros.hpp>
#include "wikore/domain/knowledge_edge.hpp"
#include "wikore/rag/knowledge_edge_repo.hpp"
#include "wikore/db.hpp"
#include <drogon/drogon.h>
#include <drogon/utils/coroutine.h>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// KnowledgeEdgeRepo integration tests.
//
// Exercises the admin edge CRUD path against a real Postgres so the CTE
// insert, the deferred two-endpoint CONSTRAINT TRIGGER, tenant scoping,
// endpoint immutability (V034 trigger), and delete-side outbox emission
// all run end-to-end. Skips without DATABASE_URL.
// ---------------------------------------------------------------------------

namespace {

constexpr auto CO_EDGE     = "1e6e0000-0000-0000-0000-0000000000c1";
constexpr auto DOC_EDGE    = "1e6e0d00-0000-0000-0000-000000000001";
constexpr auto VER_EDGE    = "1e6e7e00-0000-0000-0000-000000000001";
constexpr auto CH1_EDGE    = "1e6ec800-0000-0000-0000-000000000001";
constexpr auto CH2_EDGE    = "1e6ec800-0000-0000-0000-000000000002";
constexpr auto CH3_EDGE    = "1e6ec800-0000-0000-0000-000000000003";
constexpr auto ADMIN_USER  = "1e6ea700-0000-0000-0000-000000000001";

// Second tenant for cross-tenant isolation tests.
constexpr auto CO_OTHER    = "1e6eff00-0000-0000-0000-0000000000c1";
constexpr auto CH_OTHER    = "1e6eff00-0000-0000-0000-000000000001";
constexpr auto DOC_OTHER   = "1e6eff00-0000-0000-0000-000000000002";
constexpr auto VER_OTHER   = "1e6eff00-0000-0000-0000-000000000003";

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

void seed(drogon::orm::DbClientPtr db)
{
    exec_sync(db, "DELETE FROM companies WHERE id IN ($1::uuid, $2::uuid)",
              std::string(CO_EDGE), std::string(CO_OTHER));

    exec_sync(db,
        "INSERT INTO companies (id,name,slug) VALUES "
        "($1::uuid,'EdgeRepo','edgerepo'),($2::uuid,'Other','edgerepo-other')",
        std::string(CO_EDGE), std::string(CO_OTHER));
    auto root1 = std::string(exec_sync(db,
        "SELECT id FROM org_units WHERE company_id=$1::uuid AND type='root'",
        std::string(CO_EDGE))[0]["id"].c_str());
    auto root2 = std::string(exec_sync(db,
        "SELECT id FROM org_units WHERE company_id=$1::uuid AND type='root'",
        std::string(CO_OTHER))[0]["id"].c_str());

    exec_sync(db,
        "INSERT INTO users (id,company_id,external_sub,email,is_admin) "
        "VALUES ($1::uuid,$2::uuid,'sub-admin','admin@edgerepo.example',true)",
        std::string(ADMIN_USER), std::string(CO_EDGE));

    // Company 1: doc + 3 chunks
    exec_sync(db,
        "INSERT INTO documents (id,company_id,owner_org_unit_id,filename,title,mime_type) "
        "VALUES ($1::uuid,$2::uuid,$3::uuid,'r.txt','R','text/plain')",
        std::string(DOC_EDGE), std::string(CO_EDGE), root1);
    exec_sync(db,
        "INSERT INTO document_versions (id,company_id,document_id,version_no,source_hash,"
        "ingest_status,completed_at,activated_at,chunk_count,lifecycle_status) "
        "VALUES ($1::uuid,$2::uuid,$3::uuid,1,'h','done',now(),now(),3,'active')",
        std::string(VER_EDGE), std::string(CO_EDGE), std::string(DOC_EDGE));
    exec_sync(db,
        "INSERT INTO document_chunks (id,company_id,document_version_id,chunk_index,content,content_hash) "
        "VALUES ($1::uuid,$4::uuid,$5::uuid,0,'a','ha'),"
        "       ($2::uuid,$4::uuid,$5::uuid,1,'b','hb'),"
        "       ($3::uuid,$4::uuid,$5::uuid,2,'c','hc')",
        std::string(CH1_EDGE), std::string(CH2_EDGE), std::string(CH3_EDGE),
        std::string(CO_EDGE),  std::string(VER_EDGE));

    // Company 2: doc + 1 chunk for cross-tenant test
    exec_sync(db,
        "INSERT INTO documents (id,company_id,owner_org_unit_id,filename,title,mime_type) "
        "VALUES ($1::uuid,$2::uuid,$3::uuid,'o.txt','O','text/plain')",
        std::string(DOC_OTHER), std::string(CO_OTHER), root2);
    exec_sync(db,
        "INSERT INTO document_versions (id,company_id,document_id,version_no,source_hash,"
        "ingest_status,completed_at,activated_at,chunk_count,lifecycle_status) "
        "VALUES ($1::uuid,$2::uuid,$3::uuid,1,'h','done',now(),now(),1,'active')",
        std::string(VER_OTHER), std::string(CO_OTHER), std::string(DOC_OTHER));
    exec_sync(db,
        "INSERT INTO document_chunks (id,company_id,document_version_id,chunk_index,content,content_hash) "
        "VALUES ($1::uuid,$2::uuid,$3::uuid,0,'x','hx')",
        std::string(CH_OTHER), std::string(CO_OTHER), std::string(VER_OTHER));
}

wikore::domain::CreateKnowledgeEdgeCmd default_cmd(
    const std::string& ch1 = std::string(CH1_EDGE),
    const std::string& ch2 = std::string(CH2_EDGE),
    wikore::domain::EdgeType et = wikore::domain::EdgeType::implements)
{
    return {
        .edge_type       = et,
        .direction       = wikore::domain::EdgeDirection::directed,
        .confidence      = 0.85,
        .origin          = wikore::domain::EdgeOrigin::administrator,
        .review_state    = std::nullopt,   // defaults to accepted
        .provenance_json = "{}",
        .expires_at      = std::nullopt,
        .endpoint_0      = {.ordinal = 0, .chunk_id = ch1,
                            .role    = wikore::domain::EndpointRole::source},
        .endpoint_1      = {.ordinal = 1, .chunk_id = ch2,
                            .role    = wikore::domain::EndpointRole::target},
    };
}

} // namespace

TEST_CASE("KnowledgeEdgeRepo: create + get round-trip",
          "[integration][edge_repo]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    seed(db);

    wikore::rag::KnowledgeEdgeRepo repo(db);
    auto created = drogon::sync_wait(
        repo.create(CO_EDGE, std::string(ADMIN_USER), default_cmd()));
    REQUIRE(created.has_value());
    CHECK(created->edge_type == wikore::domain::EdgeType::implements);
    CHECK(created->direction == wikore::domain::EdgeDirection::directed);
    CHECK(created->confidence == 0.85);
    CHECK(created->review_state == wikore::domain::EdgeReviewState::accepted);
    REQUIRE(created->endpoints.size() == 2);
    CHECK(created->endpoints[0].chunk_id == CH1_EDGE);
    CHECK(created->endpoints[1].chunk_id == CH2_EDGE);
    // Admin path defaults review_state to accepted, so reviewed_by is stamped.
    REQUIRE(created->reviewed_by.has_value());
    CHECK(*created->reviewed_by == ADMIN_USER);

    auto fetched = drogon::sync_wait(repo.get(CO_EDGE, created->id));
    REQUIRE(fetched.has_value());
    CHECK(fetched->id == created->id);
    REQUIRE(fetched->endpoints.size() == 2);
}

TEST_CASE("KnowledgeEdgeRepo: create rejects self-loop and out-of-range confidence",
          "[integration][edge_repo]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    seed(db);
    wikore::rag::KnowledgeEdgeRepo repo(db);

    auto cmd = default_cmd(CH1_EDGE, CH1_EDGE);   // self-loop
    auto r = drogon::sync_wait(repo.create(CO_EDGE, ADMIN_USER, cmd));
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().kind == wikore::Error::Kind::InvalidInput);

    auto cmd2 = default_cmd();
    cmd2.confidence = 1.5;
    auto r2 = drogon::sync_wait(repo.create(CO_EDGE, ADMIN_USER, cmd2));
    REQUIRE_FALSE(r2.has_value());
    CHECK(r2.error().kind == wikore::Error::Kind::InvalidInput);
}

TEST_CASE("KnowledgeEdgeRepo: cross-tenant chunks in an edge return InvalidInput",
          "[integration][edge_repo]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    seed(db);
    wikore::rag::KnowledgeEdgeRepo repo(db);

    // company CO_EDGE + one chunk from CO_OTHER → composite FK on
    // knowledge_edge_endpoints(company_id, chunk_id) fires. Named in
    // error_mapper as knowledge_edge_endpoints_company_id_chunk_id_fkey
    // -> invalid_input, so the API returns 400, not 500.
    auto cmd = default_cmd(CH1_EDGE, CH_OTHER);
    auto r = drogon::sync_wait(repo.create(CO_EDGE, ADMIN_USER, cmd));
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().kind == wikore::Error::Kind::InvalidInput);
}

TEST_CASE("KnowledgeEdgeRepo: round-trips a non-overlap V1 edge type",
          "[integration][edge_repo]")
{
    // Regression for a review finding: the C++ EdgeType table must cover
    // every V034 CHECK vocabulary value, not just the first four.
    // 'cites' was one of the five real V1 types that were missing from
    // the initial PR.
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    seed(db);
    wikore::rag::KnowledgeEdgeRepo repo(db);

    auto cmd = default_cmd(CH1_EDGE, CH2_EDGE, wikore::domain::EdgeType::cites);
    auto created = drogon::sync_wait(repo.create(CO_EDGE, ADMIN_USER, cmd));
    REQUIRE(created.has_value());
    CHECK(created->edge_type == wikore::domain::EdgeType::cites);

    // Round-trip through get() — the hydrator must NOT substitute a
    // fallback type for a value it does not recognise.
    auto fetched = drogon::sync_wait(repo.get(CO_EDGE, created->id));
    REQUIRE(fetched.has_value());
    CHECK(fetched->edge_type == wikore::domain::EdgeType::cites);

    // Filter by exception_to (also non-overlap): should return no rows,
    // NOT a database error.
    wikore::domain::ListKnowledgeEdgesFilter f;
    f.edge_type = wikore::domain::EdgeType::exception_to;
    auto list = drogon::sync_wait(repo.list(CO_EDGE, f));
    REQUIRE(list.has_value());
    CHECK(list->empty());
}

TEST_CASE("KnowledgeEdgeRepo: hydrate fails loud on schema drift",
          "[integration][edge_repo]")
{
    // Force a row into an unknown edge_type by dropping the CHECK, so we
    // can prove hydrate_edge returns database_error rather than silently
    // rendering the row as some default (which would misrepresent stored
    // data on future migrations).
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    seed(db);
    wikore::rag::KnowledgeEdgeRepo repo(db);

    auto created = drogon::sync_wait(
        repo.create(CO_EDGE, ADMIN_USER, default_cmd()));
    REQUIRE(created.has_value());

    // Round-trip works with the correct enum.
    auto ok = drogon::sync_wait(repo.get(CO_EDGE, created->id));
    REQUIRE(ok.has_value());

    // Now corrupt the row so hydrate encounters an unknown type. We
    // temporarily drop the CHECK for the UPDATE, then restore it.
    exec_sync(db, "ALTER TABLE knowledge_edges DROP CONSTRAINT knowledge_edges_edge_type_v1_chk");
    exec_sync(db,
        "UPDATE knowledge_edges SET edge_type='future_type_not_in_c++' "
        "WHERE id=$1::uuid", created->id);
    exec_sync(db,
        "ALTER TABLE knowledge_edges ADD CONSTRAINT knowledge_edges_edge_type_v1_chk "
        "CHECK (edge_type IN ('implements','depends_on','exception_to','contradicts',"
        "                     'same_requirement_as','derived_from','cites','affects',"
        "                     'requires_approval_from','future_type_not_in_c++'))");

    auto drift = drogon::sync_wait(repo.get(CO_EDGE, created->id));
    REQUIRE_FALSE(drift.has_value());
    CHECK(drift.error().kind == wikore::Error::Kind::DatabaseError);

    // Cleanup: restore the original CHECK.
    exec_sync(db, "ALTER TABLE knowledge_edges DROP CONSTRAINT knowledge_edges_edge_type_v1_chk");
    exec_sync(db,
        "UPDATE knowledge_edges SET edge_type='implements' WHERE id=$1::uuid", created->id);
    exec_sync(db,
        "ALTER TABLE knowledge_edges ADD CONSTRAINT knowledge_edges_edge_type_v1_chk "
        "CHECK (edge_type IN ('implements','depends_on','exception_to','contradicts',"
        "                     'same_requirement_as','derived_from','cites','affects',"
        "                     'requires_approval_from'))");
}

TEST_CASE("KnowledgeEdgeRepo: get + list are tenant-scoped",
          "[integration][edge_repo]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    seed(db);
    wikore::rag::KnowledgeEdgeRepo repo(db);

    auto e = drogon::sync_wait(
        repo.create(CO_EDGE, ADMIN_USER, default_cmd()));
    REQUIRE(e.has_value());

    // Get from the OTHER company must return not_found (existence hidden).
    auto miss = drogon::sync_wait(repo.get(CO_OTHER, e->id));
    REQUIRE_FALSE(miss.has_value());
    CHECK(miss.error().kind == wikore::Error::Kind::NotFound);

    // List from the OTHER company must not include our edge.
    auto other_list = drogon::sync_wait(
        repo.list(CO_OTHER, wikore::domain::ListKnowledgeEdgesFilter{}));
    REQUIRE(other_list.has_value());
    for (const auto& row : *other_list)
        CHECK(row.id != e->id);

    auto own_list = drogon::sync_wait(
        repo.list(CO_EDGE, wikore::domain::ListKnowledgeEdgesFilter{}));
    REQUIRE(own_list.has_value());
    bool found = false;
    for (const auto& row : *own_list) if (row.id == e->id) { found = true; break; }
    CHECK(found);
}

TEST_CASE("KnowledgeEdgeRepo: list filter by chunk_id matches either endpoint",
          "[integration][edge_repo]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    seed(db);
    wikore::rag::KnowledgeEdgeRepo repo(db);

    auto a = drogon::sync_wait(
        repo.create(CO_EDGE, ADMIN_USER, default_cmd(CH1_EDGE, CH2_EDGE)));
    auto b = drogon::sync_wait(
        repo.create(CO_EDGE, ADMIN_USER, default_cmd(CH2_EDGE, CH3_EDGE,
                                                     wikore::domain::EdgeType::depends_on)));
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());

    wikore::domain::ListKnowledgeEdgesFilter f;
    f.endpoint_chunk_id = std::string(CH2_EDGE);
    auto list = drogon::sync_wait(repo.list(CO_EDGE, f));
    REQUIRE(list.has_value());
    // Both edges touch CH2_EDGE.
    int matches = 0;
    for (const auto& row : *list)
        if (row.id == a->id || row.id == b->id) ++matches;
    CHECK(matches == 2);
}

TEST_CASE("KnowledgeEdgeRepo: update rejects out-of-range confidence and no-ops for empty patch",
          "[integration][edge_repo]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    seed(db);
    wikore::rag::KnowledgeEdgeRepo repo(db);

    auto e = drogon::sync_wait(repo.create(CO_EDGE, ADMIN_USER, default_cmd()));
    REQUIRE(e.has_value());

    wikore::domain::UpdateKnowledgeEdgeCmd bad;
    bad.confidence = 2.0;
    auto rbad = drogon::sync_wait(repo.update(CO_EDGE, ADMIN_USER, e->id, bad));
    REQUIRE_FALSE(rbad.has_value());
    CHECK(rbad.error().kind == wikore::Error::Kind::InvalidInput);

    wikore::domain::UpdateKnowledgeEdgeCmd empty;
    auto rempty = drogon::sync_wait(repo.update(CO_EDGE, ADMIN_USER, e->id, empty));
    REQUIRE(rempty.has_value());
    // No-op keeps edge_version unchanged.
    CHECK(rempty->edge_version == e->edge_version);
}

TEST_CASE("KnowledgeEdgeRepo: update bumps edge_version and stamps reviewer",
          "[integration][edge_repo]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    seed(db);
    wikore::rag::KnowledgeEdgeRepo repo(db);

    // Create with review_state=proposed so we can move it to accepted.
    auto cmd = default_cmd();
    cmd.review_state = wikore::domain::EdgeReviewState::proposed;
    auto e = drogon::sync_wait(repo.create(CO_EDGE, ADMIN_USER, cmd));
    REQUIRE(e.has_value());
    CHECK_FALSE(e->reviewed_by.has_value());

    wikore::domain::UpdateKnowledgeEdgeCmd upd;
    upd.confidence   = 0.9;
    upd.review_state = wikore::domain::EdgeReviewState::accepted;
    auto after = drogon::sync_wait(repo.update(CO_EDGE, ADMIN_USER, e->id, upd));
    REQUIRE(after.has_value());
    CHECK(after->edge_version == e->edge_version + 1);
    CHECK(after->confidence == 0.9);
    CHECK(after->review_state == wikore::domain::EdgeReviewState::accepted);
    REQUIRE(after->reviewed_by.has_value());
    CHECK(*after->reviewed_by == ADMIN_USER);
    REQUIRE(after->reviewed_at.has_value());
    // Not superseded — timestamp stays NULL.
    CHECK_FALSE(after->superseded_at.has_value());
}

TEST_CASE("KnowledgeEdgeRepo: update to superseded stamps superseded_at",
          "[integration][edge_repo]")
{
    // Regression for a review finding: V034's history captures
    // edge_superseded_at for as-of reconstruction. A superseded edge
    // with a NULL superseded_at undermines exactly the lifecycle data
    // the schema was built to keep. Stamp it whenever review_state
    // moves to superseded, on both create() and update().
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    seed(db);
    wikore::rag::KnowledgeEdgeRepo repo(db);

    auto cmd = default_cmd();
    cmd.review_state = wikore::domain::EdgeReviewState::proposed;
    auto e = drogon::sync_wait(repo.create(CO_EDGE, ADMIN_USER, cmd));
    REQUIRE(e.has_value());
    CHECK_FALSE(e->superseded_at.has_value());

    wikore::domain::UpdateKnowledgeEdgeCmd upd;
    upd.review_state = wikore::domain::EdgeReviewState::superseded;
    auto after = drogon::sync_wait(repo.update(CO_EDGE, ADMIN_USER, e->id, upd));
    REQUIRE(after.has_value());
    CHECK(after->review_state == wikore::domain::EdgeReviewState::superseded);
    REQUIRE(after->superseded_at.has_value());
    REQUIRE(after->reviewed_by.has_value());
    CHECK(*after->reviewed_by == ADMIN_USER);
}

TEST_CASE("KnowledgeEdgeRepo: create with malformed provenance/expires_at returns InvalidInput",
          "[integration][edge_repo]")
{
    // Regression for a review finding: user-controlled cast failures
    // (22P02, 22007, 22032) must return 400, not 500 — routine admin
    // input mistakes should not look like server errors.
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    seed(db);
    wikore::rag::KnowledgeEdgeRepo repo(db);

    // Malformed JSON in provenance — hits ::jsonb cast, SQLSTATE 22P02
    // (invalid_text_representation from json parse).
    auto bad_json = default_cmd();
    bad_json.provenance_json = "{not valid json";
    auto r1 = drogon::sync_wait(repo.create(CO_EDGE, ADMIN_USER, bad_json));
    REQUIRE_FALSE(r1.has_value());
    CHECK(r1.error().kind == wikore::Error::Kind::InvalidInput);

    // Garbage expires_at — hits ::timestamptz cast, SQLSTATE 22007.
    auto bad_ts = default_cmd();
    bad_ts.expires_at = "not-a-timestamp";
    auto r2 = drogon::sync_wait(repo.create(CO_EDGE, ADMIN_USER, bad_ts));
    REQUIRE_FALSE(r2.has_value());
    CHECK(r2.error().kind == wikore::Error::Kind::InvalidInput);
}

TEST_CASE("KnowledgeEdgeRepo: create with review_state=superseded stamps superseded_at",
          "[integration][edge_repo]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    seed(db);
    wikore::rag::KnowledgeEdgeRepo repo(db);

    auto cmd = default_cmd();
    cmd.review_state = wikore::domain::EdgeReviewState::superseded;
    auto e = drogon::sync_wait(repo.create(CO_EDGE, ADMIN_USER, cmd));
    REQUIRE(e.has_value());
    CHECK(e->review_state == wikore::domain::EdgeReviewState::superseded);
    REQUIRE(e->superseded_at.has_value());
}

TEST_CASE("KnowledgeEdgeRepo: remove enqueues qdrant_delete_edge_points via V034 trigger",
          "[integration][edge_repo]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    seed(db);
    wikore::rag::KnowledgeEdgeRepo repo(db);

    auto e = drogon::sync_wait(repo.create(CO_EDGE, ADMIN_USER, default_cmd()));
    REQUIRE(e.has_value());

    // Register a global embedding model and insert one edge embedding so the
    // BEFORE DELETE trigger has a point ID to emit into the outbox.
    exec_sync(db,
        "INSERT INTO embedding_models (id,name,qdrant_collection,dimension) "
        "VALUES ('1e6eee00-0000-0000-0000-000000000001','edgerepo-model',"
        "        'wikore_edgerepo_test',4) "
        "ON CONFLICT (id) DO NOTHING");
    exec_sync(db,
        "INSERT INTO knowledge_edge_embeddings "
        "(company_id,edge_id,embedding_model_id,qdrant_point_id,formula_version,indexed_edge_version) "
        "VALUES ($1::uuid,$2::uuid,'1e6eee00-0000-0000-0000-000000000001',"
        "        '1e6eff77-0000-0000-0000-000000000001',1,1)",
        std::string(CO_EDGE), e->id);

    auto rm = drogon::sync_wait(repo.remove(CO_EDGE, e->id));
    REQUIRE(rm.has_value());

    // Row is gone.
    auto miss = drogon::sync_wait(repo.get(CO_EDGE, e->id));
    REQUIRE_FALSE(miss.has_value());
    CHECK(miss.error().kind == wikore::Error::Kind::NotFound);

    // Outbox event materialized with the point ID.
    auto ev = exec_sync(db,
        "SELECT payload->'qdrant_point_ids' @> to_jsonb(ARRAY['1e6eff77-0000-0000-0000-000000000001'::uuid]) AS has_point "
        "FROM outbox_events "
        "WHERE aggregate_id=$1::uuid AND job_type='qdrant_delete_edge_points'",
        e->id);
    REQUIRE(ev.size() == 1);
    CHECK(std::string(ev[0]["has_point"].c_str()) == "t");
}

TEST_CASE("KnowledgeEdgeRepo: remove is tenant-scoped (other tenant sees not_found)",
          "[integration][edge_repo]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    seed(db);
    wikore::rag::KnowledgeEdgeRepo repo(db);

    auto e = drogon::sync_wait(repo.create(CO_EDGE, ADMIN_USER, default_cmd()));
    REQUIRE(e.has_value());
    auto rm = drogon::sync_wait(repo.remove(CO_OTHER, e->id));
    REQUIRE_FALSE(rm.has_value());
    CHECK(rm.error().kind == wikore::Error::Kind::NotFound);
    // Still present under its own tenant.
    auto still = drogon::sync_wait(repo.get(CO_EDGE, e->id));
    CHECK(still.has_value());
}
