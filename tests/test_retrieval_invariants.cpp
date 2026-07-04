#include <catch2/catch_test_macros.hpp>
#include "wikore/access.hpp"
#include "wikore/access_resolver.hpp"
#include "wikore/ingest/document_repo.hpp"
#include "wikore/rag/evidence_gate.hpp"
#include "wikore/db.hpp"
#include <drogon/drogon.h>
#include <drogon/utils/coroutine.h>
#include <algorithm>
#include <cstdlib>
#include <map>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// Iteration 2 / section-5 item 1: G1 subset property test.
//
// G1 (gate authority): a chunk reaches the reranker only if a single
// authoritative Postgres query says the reader may see it. Under option (a)
// the gate resolves the resource axis live from resource_grants, so the gate
// output must equal the authoritative truth for ANY grant/membership/tree
// configuration. We assert that across randomized configurations:
//
//   gate(reader, candidates)  ==  oracle(reader, candidates)
//
// where:
//   * gate   = the ACTUAL EvidenceGate (its production set-based query), so
//              this test validates the real boundary, not a copy of the SQL.
//   * oracle = an INDEPENDENT formulation built from two pre-existing,
//              separately-written resolvers:
//                - PostgresDocumentRepo::fetch_access_scopes(doc) -> the
//                  resource-side expansion (which OUs can see the doc), and
//                - AccessService::effective_read_orgs(reader)     -> the
//                  reader-side scope.
//              A doc is visible iff those two sets intersect.
//
// Equality (not just subset) is the right assertion here: because the gate
// reads live Postgres, there is no overlay lag at the gate layer, so safety
// (gate is a subset of truth = no leak) and liveness (gate equals truth once
// consistent) collapse to a single equality check per configuration. The
// W1/W2/W3 prefilter-staleness interleaving test layers on top of this once
// EvidenceGate + the section-2 overlay exist; this test pins the boundary.
//
// History: the first cut of the section-0 query matched principal_id against
// reader_grant_keys (reader_scope + ancestors) for ALL grants, which silently
// ignored principal_applies_to and over-granted self_only-principal grants to
// readers sitting in a descendant of the principal. This test was written to
// catch exactly that class of divergence between the reader-side inversion and
// the resource-side truth.
// ---------------------------------------------------------------------------

namespace {

constexpr auto CO = "61c00000-0000-0000-0000-0000000000a1"; // "g1 co"

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

struct Trial {
    std::vector<std::string> ous;      // all org_unit ids (incl root at [0])
    std::vector<std::string> docs;     // document ids
    std::vector<std::string> versions; // active version id per doc (index-aligned)
    std::vector<std::string> chunks;   // chunk id per doc (index-aligned)
    std::vector<std::string> users;    // user ids
};

// Build a random tenant: tree, documents (+active version +1 chunk each),
// grants, memberships. Returns the ids needed to drive the assertions.
Trial build_random_trial(drogon::orm::DbClientPtr db, std::mt19937& rng)
{
    exec_sync(db, "DELETE FROM companies WHERE id=$1::uuid", std::string(CO));
    exec_sync(db,
        "INSERT INTO companies (id, name, slug) VALUES ($1::uuid, 'G1', 'g1')",
        std::string(CO));

    Trial t;
    auto root = exec_sync(db,
        "SELECT id FROM org_units WHERE company_id=$1::uuid AND type='root'",
        std::string(CO));
    t.ous.push_back(std::string(root[0]["id"].c_str()));

    // Random tree: each new OU hangs off a random existing OU.
    const int n_ou = 6 + (rng() % 8);              // 6..13 OUs
    for (int i = 0; i < n_ou; ++i) {
        const auto& parent = t.ous[rng() % t.ous.size()];
        auto r = exec_sync(db,
            "INSERT INTO org_units (company_id, parent_id, type, slug, name) "
            "VALUES ($1::uuid, $2::uuid, 'team', $3, $3) RETURNING id",
            std::string(CO), parent, std::string("ou") + std::to_string(i));
        t.ous.push_back(std::string(r[0]["id"].c_str()));
    }

    // Documents: each owned by a random OU, one active version, one chunk.
    const int n_doc = 3 + (rng() % 4);             // 3..6 docs
    for (int i = 0; i < n_doc; ++i) {
        const auto& owner = t.ous[rng() % t.ous.size()];
        auto d = exec_sync(db,
            "INSERT INTO documents (company_id, owner_org_unit_id, filename) "
            "VALUES ($1::uuid, $2::uuid, $3) RETURNING id",
            std::string(CO), owner, std::string("d") + std::to_string(i) + ".txt");
        const auto doc_id = std::string(d[0]["id"].c_str());
        t.docs.push_back(doc_id);
        auto v = exec_sync(db,
            "INSERT INTO document_versions "
            "(company_id, document_id, version_no, source_hash, ingest_status, "
            " completed_at, activated_at, chunk_count, lifecycle_status) "
            "VALUES ($1::uuid, $2::uuid, 1, 'h', 'done', now(), now(), 1, 'active') "
            "RETURNING id",
            std::string(CO), doc_id);
        const auto ver_id = std::string(v[0]["id"].c_str());
        t.versions.push_back(ver_id);
        auto c = exec_sync(db,
            "INSERT INTO document_chunks "
            "(company_id, document_version_id, chunk_index, content, content_hash, "
            " qdrant_prefilter_scope_ids) "
            "VALUES ($1::uuid, $2::uuid, 0, 'x', $3, '{}') RETURNING id",
            std::string(CO), ver_id, std::string("h") + doc_id);
        t.chunks.push_back(std::string(c[0]["id"].c_str()));
    }

    // Grants: random resource (a document or an org_unit), random principal OU,
    // random applies_to on both axes. permission read.
    const char* applies[] = {"self_only", "self_and_descendants"};
    const int n_grant = 4 + (rng() % 5);           // 4..8 grants
    for (int i = 0; i < n_grant; ++i) {
        const auto& principal = t.ous[rng() % t.ous.size()];
        const char* p_applies = applies[rng() % 2];
        if (rng() % 2 == 0 && !t.docs.empty()) {
            // document-resource grant (resource_applies_to forced self_only)
            const auto& res = t.docs[rng() % t.docs.size()];
            exec_sync(db,
                "INSERT INTO resource_grants "
                "(company_id, resource_type, resource_id, resource_applies_to, "
                " principal_type, principal_id, principal_applies_to, permission) "
                "VALUES ($1::uuid,'document',$2::uuid,'self_only','org_unit',$3::uuid,$4,'read') "
                "ON CONFLICT DO NOTHING",
                std::string(CO), res, principal, std::string(p_applies));
        } else {
            const auto& res = t.ous[rng() % t.ous.size()];
            const char* r_applies = applies[rng() % 2];
            exec_sync(db,
                "INSERT INTO resource_grants "
                "(company_id, resource_type, resource_id, resource_applies_to, "
                " principal_type, principal_id, principal_applies_to, permission) "
                "VALUES ($1::uuid,'org_unit',$2::uuid,$3,'org_unit',$4::uuid,$5,'read') "
                "ON CONFLICT DO NOTHING",
                std::string(CO), res, std::string(r_applies),
                principal, std::string(p_applies));
        }
    }

    // Two groups, each with a membership on a random OU (principal = group).
    // Users added to a group inherit its org_unit membership, exercising the
    // group-derived arm of the reader-scope resolvers.
    std::vector<std::string> groups;
    for (int g = 0; g < 2; ++g) {
        const auto gid = std::string(exec_sync(db,
            "INSERT INTO groups (company_id, name) VALUES ($1::uuid, $2) RETURNING id",
            std::string(CO), std::string("grp") + std::to_string(g))[0]["id"].c_str());
        groups.push_back(gid);
        exec_sync(db,
            "INSERT INTO memberships (company_id, group_id, org_unit_id, role, applies_to) "
            "VALUES ($1::uuid, $2::uuid, $3::uuid, 'viewer', $4) ON CONFLICT DO NOTHING",
            std::string(CO), gid, t.ous[rng() % t.ous.size()], std::string(applies[rng() % 2]));
    }

    // Users with 1-2 direct memberships each, ~half also placed in a group.
    const int n_user = 3 + (rng() % 3);            // 3..5 users
    for (int i = 0; i < n_user; ++i) {
        auto u = exec_sync(db,
            "INSERT INTO users (company_id, external_sub, email) "
            "VALUES ($1::uuid, $2, $3) RETURNING id",
            std::string(CO), std::string("sub") + std::to_string(i),
            std::string("u") + std::to_string(i) + "@g1.test");
        const auto uid = std::string(u[0]["id"].c_str());
        t.users.push_back(uid);
        const int n_mem = 1 + (rng() % 2);
        for (int m = 0; m < n_mem; ++m) {
            const auto& ou = t.ous[rng() % t.ous.size()];
            exec_sync(db,
                "INSERT INTO memberships (company_id, user_id, org_unit_id, role, applies_to) "
                "VALUES ($1::uuid, $2::uuid, $3::uuid, 'viewer', $4) ON CONFLICT DO NOTHING",
                std::string(CO), uid, ou, std::string(applies[rng() % 2]));
        }
        if (rng() % 2 == 0)
            exec_sync(db,
                "INSERT INTO group_members (company_id, group_id, user_id) "
                "VALUES ($1::uuid, $2::uuid, $3::uuid) ON CONFLICT DO NOTHING",
                std::string(CO), groups[rng() % groups.size()], uid);
    }
    return t;
}

} // namespace

TEST_CASE("G1 property: EvidenceGate output equals authoritative PG truth across random configs",
          "[integration][retrieval][g1]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    wikore::AccessService                access(db);   // oracle reader-side
    wikore::ingest::PostgresDocumentRepo repo(db);     // oracle resource-side
    wikore::PostgresAccessResolver       resolver(db); // PRODUCTION reader-side (under test)
    wikore::rag::EvidenceGate            gate(db);

    // Full clearance + all fixtures active, so lifecycle/sensitivity never drop
    // anything and the gate output reflects purely the resource+reader axes -
    // exactly what the oracle computes.
    const std::vector<std::string> full_clearance{"public","internal","confidential","restricted"};

    constexpr unsigned kTrials = 40;
    for (unsigned seed = 1; seed <= kTrials; ++seed) {
        std::mt19937 rng(seed);
        Trial t = build_random_trial(db, rng);
        const auto& root = t.ous[0];

        // Candidate set = every chunk; doc -> its single chunk (index-aligned).
        std::map<std::string, std::string> doc_chunk;
        std::vector<wikore::rag::ChunkCandidate> candidates;
        candidates.reserve(t.docs.size());
        for (size_t i = 0; i < t.docs.size(); ++i) {
            doc_chunk[t.docs[i]] = t.chunks[i];
            candidates.push_back({.chunk_id = t.chunks[i],
                                  .document_version_id = t.versions[i],
                                  .score = 1.0f, .payload = {}});
        }

        for (const auto& user : t.users) {
            // ---- ORACLE reader-side: effective_read_orgs (independent copy) ----
            auto oracle_scope = drogon::sync_wait(access.effective_read_orgs(CO, user, root));
            std::set<std::string> scope_set(oracle_scope.begin(), oracle_scope.end());
            std::set<std::string> oracle_chunks;
            for (const auto& doc : t.docs) {
                auto res = drogon::sync_wait(repo.fetch_access_scopes(CO, doc));
                REQUIRE(res.has_value());
                for (const auto& ou : *res)
                    if (scope_set.count(ou)) { oracle_chunks.insert(doc_chunk[doc]); break; }
            }

            // ---- GATE fed by the PRODUCTION resolver: validates both the
            // resolver's scope SQL AND the gate against the oracle at once. If
            // the resolver diverges from effective_read_orgs, the sets differ. --
            auto rr = drogon::sync_wait(resolver.resolve(CO, user, root));
            REQUIRE(rr.has_value());
            wikore::AccessScope scope; scope.org_unit_ids = rr->org_unit_ids;
            auto allowed = drogon::sync_wait(gate.evaluate(CO, scope, full_clearance, candidates));
            REQUIRE(allowed.has_value());
            std::set<std::string> gate_chunks;
            for (const auto& a : *allowed) gate_chunks.insert(a.chunk_id());

            // ---- assert equality (safety + liveness) ----
            INFO("seed=" << seed << " user=" << user
                 << " oracle_scope=" << oracle_scope.size()
                 << " resolver_scope=" << rr->org_unit_ids.size());
            for (const auto& c : gate_chunks)
                if (!oracle_chunks.count(c))
                    FAIL("OVER-GRANT (leak): gate emitted chunk " << c
                         << " not in PG truth (seed=" << seed << " user=" << user << ")");
            for (const auto& c : oracle_chunks)
                if (!gate_chunks.count(c))
                    FAIL("under-grant (recall): gate dropped chunk " << c
                         << " that PG truth allows (seed=" << seed << " user=" << user << ")");
            CHECK(gate_chunks == oracle_chunks);
        }
    }
}

// ---------------------------------------------------------------------------
// Scenario F (section-5 item 2): a lost lr:eff invalidation cannot leak.
//
// A membership revoke whose lr:eff DEL is lost would leave a stale-wide
// cached reader scope; feeding that to the gate would leak. The §2.6 fix is
// users.scope_epoch: V032's membership trigger bumps it in the same tx as the
// revoke, so a stale cache entry (carrying the old epoch) is DETECTABLY stale
// and forces a re-resolve. This test pins both halves of the backstop without
// the not-yet-built cache layer:
//   (1) the revoke bumps users.scope_epoch (old != new -> stale cache caught),
//   (2) the re-resolved scope excludes the revoked OU, so the gate fed the
//       fresh scope returns nothing (no leak).
// It should fail if V032's scope_epoch coverage of membership changes is ever
// removed (the stale cache would then be undetectable).
// ---------------------------------------------------------------------------
TEST_CASE("G1 scenario F: revoke bumps scope_epoch and re-resolve is leak-free",
          "[integration][retrieval][g1]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    wikore::AccessService access(db);

    exec_sync(db, "DELETE FROM companies WHERE id=$1::uuid", std::string(CO));
    exec_sync(db, "INSERT INTO companies (id,name,slug) VALUES ($1::uuid,'F','f')", std::string(CO));
    const auto root = std::string(exec_sync(db,
        "SELECT id FROM org_units WHERE company_id=$1::uuid AND type='root'",
        std::string(CO))[0]["id"].c_str());
    const auto hr = std::string(exec_sync(db,
        "INSERT INTO org_units (company_id,parent_id,type,slug,name) "
        "VALUES ($1::uuid,$2::uuid,'department','hr','HR') RETURNING id",
        std::string(CO), root)[0]["id"].c_str());
    const auto user = std::string(exec_sync(db,
        "INSERT INTO users (company_id,external_sub,email) "
        "VALUES ($1::uuid,'subF','f@test') RETURNING id", std::string(CO))[0]["id"].c_str());
    exec_sync(db,
        "INSERT INTO memberships (company_id,user_id,org_unit_id,role,applies_to) "
        "VALUES ($1::uuid,$2::uuid,$3::uuid,'viewer','self_and_descendants')",
        std::string(CO), user, hr);
    // an HR-owned doc + chunk
    const auto doc = std::string(exec_sync(db,
        "INSERT INTO documents (company_id,owner_org_unit_id,filename) "
        "VALUES ($1::uuid,$2::uuid,'hr.txt') RETURNING id", std::string(CO), hr)[0]["id"].c_str());
    const auto ver = std::string(exec_sync(db,
        "INSERT INTO document_versions (company_id,document_id,version_no,source_hash,"
        "ingest_status,completed_at,activated_at,chunk_count,lifecycle_status) "
        "VALUES ($1::uuid,$2::uuid,1,'h','done',now(),now(),1,'active') RETURNING id",
        std::string(CO), doc)[0]["id"].c_str());
    const auto chunk = std::string(exec_sync(db,
        "INSERT INTO document_chunks (company_id,document_version_id,chunk_index,content,"
        "content_hash,qdrant_prefilter_scope_ids) VALUES ($1::uuid,$2::uuid,0,'x','hF','{}') "
        "RETURNING id", std::string(CO), ver)[0]["id"].c_str());

    wikore::rag::EvidenceGate gate(db);
    const std::vector<std::string> sens{"public","internal","confidential","restricted"};
    const std::vector<wikore::rag::ChunkCandidate> candidates{
        {.chunk_id = chunk, .document_version_id = ver, .score = 1.0f, .payload = {}}};
    auto gate_sees_chunk = [&](const std::vector<std::string>& scope_vec) {
        wikore::AccessScope s; s.org_unit_ids = scope_vec;
        auto a = drogon::sync_wait(gate.evaluate(CO, s, sens, candidates));
        REQUIRE(a.has_value());
        return !a->empty();
    };
    auto epoch_of = [&]{
        return std::string(exec_sync(db,
            "SELECT scope_epoch::text AS e FROM users WHERE id=$1::uuid",
            std::string(user))[0]["e"].c_str());
    };

    // Pre-revoke: U is in HR, the gate returns the HR chunk.
    auto scope0 = drogon::sync_wait(access.effective_read_orgs(CO, user, root));
    const auto epoch0 = epoch_of();
    REQUIRE(gate_sees_chunk(scope0));

    // Revoke. The lr:eff DEL is "lost" (we touch no cache); only PG changes.
    exec_sync(db, "DELETE FROM memberships WHERE company_id=$1::uuid AND user_id=$2::uuid",
              std::string(CO), std::string(user));

    // (1) scope_epoch bumped -> a stale cache carrying epoch0 is now detectable.
    const auto epoch1 = epoch_of();
    INFO("epoch0=" << epoch0 << " epoch1=" << epoch1);
    CHECK(epoch1 != epoch0);

    // (2) re-resolve excludes HR, and the gate fed the fresh scope leaks nothing.
    auto scope1 = drogon::sync_wait(access.effective_read_orgs(CO, user, root));
    CHECK(std::find(scope1.begin(), scope1.end(), hr) == scope1.end());
    CHECK_FALSE(gate_sees_chunk(scope1));
}

// ---------------------------------------------------------------------------
// Scenario E (section-5 item 3): concurrent move_org_unit never tears scope.
//
// effective_read_orgs is a single SQL statement, so under Read Committed it
// sees one consistent snapshot and can never observe a half-applied move. We
// run the resolver concurrently with a move_org_unit (separate pooled
// connections, real overlap) many times and assert the resolver output is
// ALWAYS exactly the pre-move or the post-move scope, never a mix. Fails if
// the resolver is ever refactored into multiple statements.
// ---------------------------------------------------------------------------
TEST_CASE("G1 scenario E: concurrent move_org_unit yields pre- or post-move scope, never torn",
          "[integration][retrieval][g1]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    wikore::AccessService access(db);

    exec_sync(db, "DELETE FROM companies WHERE id=$1::uuid", std::string(CO));
    exec_sync(db, "INSERT INTO companies (id,name,slug) VALUES ($1::uuid,'E','e')", std::string(CO));
    const auto root = std::string(exec_sync(db,
        "SELECT id FROM org_units WHERE company_id=$1::uuid AND type='root'",
        std::string(CO))[0]["id"].c_str());
    auto mk = [&](const std::string& parent, const std::string& slug) {
        return std::string(exec_sync(db,
            "INSERT INTO org_units (company_id,parent_id,type,slug,name) "
            "VALUES ($1::uuid,$2::uuid,'team',$3,$3) RETURNING id",
            std::string(CO), parent, slug)[0]["id"].c_str());
    };
    const auto A = mk(root, "a");
    const auto B = mk(root, "b");
    const auto N = mk(A, "n");          // movable node, starts under A
    mk(N, "nc");                        // N has a child, so a torn closure read is detectable
    const auto user = std::string(exec_sync(db,
        "INSERT INTO users (company_id,external_sub,email) "
        "VALUES ($1::uuid,'subE','e@test') RETURNING id", std::string(CO))[0]["id"].c_str());
    exec_sync(db,                       // U reads everything under A (s_and_d)
        "INSERT INTO memberships (company_id,user_id,org_unit_id,role,applies_to) "
        "VALUES ($1::uuid,$2::uuid,$3::uuid,'viewer','self_and_descendants')",
        std::string(CO), user, A);

    auto resolve_set = [&]{
        auto v = drogon::sync_wait(access.effective_read_orgs(CO, user, root));
        return std::set<std::string>(v.begin(), v.end());
    };
    auto move_to = [&](const std::string& parent){
        exec_sync(db, "SELECT move_org_unit($1::uuid,$2::uuid,$3::uuid)",
                  std::string(CO), std::string(N), parent);
    };

    // The two legal outcomes (N under A includes N+NC; N under B excludes them).
    move_to(A); const auto set_A = resolve_set();
    move_to(B); const auto set_B = resolve_set();
    REQUIRE(set_A != set_B);            // the move actually changes U's scope
    move_to(A);                         // reset to known start

    std::mt19937 rng(12345);
    constexpr int kIters = 100;
    for (int i = 0; i < kIters; ++i) {
        const auto& target = (i % 2 == 0) ? B : A;   // alternate direction
        std::set<std::string> got;
        std::thread mover([&]{ move_to(target); });
        std::thread reader([&]{
            // small jitter to vary the move/resolve overlap window
            std::this_thread::sleep_for(std::chrono::microseconds(rng() % 200));
            got = resolve_set();
        });
        mover.join(); reader.join();
        INFO("iter=" << i << " target=" << (target == B ? "B" : "A"));
        const bool ok = (got == set_A) || (got == set_B);
        if (!ok)
            FAIL("TORN read: resolver output is neither the pre- nor post-move "
                 "scope (iter=" << i << ", size=" << got.size() << ")");
        CHECK(ok);
    }
}
