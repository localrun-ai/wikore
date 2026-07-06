#include <catch2/catch_test_macros.hpp>
#include "wikore/rag/relationship_vector_store.hpp"

// ---------------------------------------------------------------------------
// Unit tests for QdrantRelationshipVectorStore::parse_search_response — the
// static helper that turns a raw (status, body) pair into the same
// Result<> that search() would produce. Split out from search() so we can
// pin the 404-means-empty and non-200-means-unavailable semantics without
// standing up a live Qdrant.
// ---------------------------------------------------------------------------

using wikore::rag::QdrantRelationshipVectorStore;

TEST_CASE("QdrantRelationshipVectorStore::parse_search_response — 404 collection missing → empty",
          "[relationship_store][qdrant]")
{
    // Missing collection is a recall issue (zero edges indexed), not
    // an outage. Empty body irrelevant since 404 short-circuits.
    auto r = QdrantRelationshipVectorStore::parse_search_response(
        404, "", "tenant-A", "wikore_edges_v1");
    REQUIRE(r);
    CHECK(r->empty());
}

TEST_CASE("QdrantRelationshipVectorStore::parse_search_response — non-200 non-404 → unavailable",
          "[relationship_store][qdrant]")
{
    for (int code : {500, 502, 503, 504, 401, 403, 429}) {
        INFO("status_code=" << code);
        auto r = QdrantRelationshipVectorStore::parse_search_response(
            code, "internal error", "tenant-A", "col");
        REQUIRE_FALSE(r);
        CHECK(r.error().kind == wikore::Error::Kind::ServiceUnavailable);
    }
}

TEST_CASE("QdrantRelationshipVectorStore::parse_search_response — malformed body → unavailable",
          "[relationship_store][qdrant]")
{
    auto r = QdrantRelationshipVectorStore::parse_search_response(
        200, "{not-json", "tenant-A", "col");
    REQUIRE_FALSE(r);
    CHECK(r.error().kind == wikore::Error::Kind::ServiceUnavailable);
}

TEST_CASE("QdrantRelationshipVectorStore::parse_search_response — 200 empty result → empty vec",
          "[relationship_store][qdrant]")
{
    // Real Qdrant envelope shape: result is an array (may be empty),
    // status and time are ignored via error_on_unknown_keys=false.
    constexpr auto body = R"({"result":[],"status":"ok","time":0.001})";
    auto r = QdrantRelationshipVectorStore::parse_search_response(
        200, body, "tenant-A", "col");
    REQUIRE(r);
    CHECK(r->empty());
}

TEST_CASE("QdrantRelationshipVectorStore::parse_search_response — 200 with hit → maps payload",
          "[relationship_store][qdrant]")
{
    constexpr auto body = R"({"result":[
        {"id":"pt-1","score":0.9,"payload":{
            "company_id":"tenant-A",
            "edge_id":"edge-1",
            "edge_type":"implements",
            "edge_version":3,
            "formula_version":1,
            "endpoint_0_chunk_id":"c0",
            "endpoint_1_chunk_id":"c1",
            "confidence":0.8,
            "authority_0":"src",
            "authority_1":"tgt",
            "review_state":"accepted"
        }}
    ],"status":"ok","time":0.002})";
    auto r = QdrantRelationshipVectorStore::parse_search_response(
        200, body, "tenant-A", "col");
    REQUIRE(r);
    REQUIRE(r->size() == 1);
    CHECK((*r)[0].edge_id      == "edge-1");
    CHECK((*r)[0].company_id   == "tenant-A");
    CHECK((*r)[0].edge_type    == "implements");
    CHECK((*r)[0].edge_version == 3);
    CHECK((*r)[0].score        == 0.9f);
}

TEST_CASE("QdrantRelationshipVectorStore::parse_search_response — cross-tenant hit is dropped",
          "[relationship_store][qdrant]")
{
    // Defense-in-depth: if Qdrant ever returned an edge for a
    // different tenant (bug upstream), the parser must NOT surface it.
    constexpr auto body = R"({"result":[
        {"id":"pt-1","score":0.9,"payload":{
            "company_id":"tenant-B",
            "edge_id":"edge-1",
            "edge_type":"implements",
            "edge_version":1,
            "formula_version":1,
            "endpoint_0_chunk_id":"c0",
            "endpoint_1_chunk_id":"c1",
            "confidence":0.8,
            "authority_0":"src",
            "authority_1":"tgt",
            "review_state":"accepted"
        }}
    ],"status":"ok","time":0.002})";
    auto r = QdrantRelationshipVectorStore::parse_search_response(
        200, body, "tenant-A", "col");
    REQUIRE(r);
    CHECK(r->empty());
}
