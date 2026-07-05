#include "wikore/rag/relationship_evidence_gate.hpp"
#include "wikore/adapters/postgres/error_mapper.hpp"
#include <drogon/drogon.h>
#include <string>
#include <unordered_map>
#include <utility>

namespace wikore::rag {

namespace {

std::string pg_array(const std::vector<std::string>& v)
{
    std::string out = "{";
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i) out += ',';
        out += '"';
        out += v[i];
        out += '"';
    }
    out += '}';
    return out;
}

// ---------------------------------------------------------------------------
// G2 gate query. One set-based statement; runs once per batch.
//
// Structure (see docs/barygraph_features.md §"Relationship EvidenceGate
// design" → SQL shape):
//
//   * candidate_edges         — the caller's advisory (edge_id) list.
//   * edge_records            — tenant + state filter on knowledge_edges.
//   * required_endpoints      — pull ordinal 0 and 1 for each surviving
//                               edge from knowledge_edge_endpoints.
//   * reader_scope /
//     reader_grant_keys       — reused from G1: the caller's org-unit
//                               scope, closed upward through
//                               org_unit_closure so a grant to an
//                               ancestor covers descendants.
//   * cand_docs               — owning documents of every endpoint's
//                               chunk, filtered by lifecycle +
//                               sensitivity.
//   * visible                 — arm 1 (owner-in-scope), arm 2 (grant
//                               with principal_type='org_unit'), arm 3
//                               (grant on an ancestor org_unit whose
//                               subtree contains the doc's owner).
//                               EXACTLY the three arms G1 uses; ANY
//                               drift between G1 and G2 is a security
//                               bug. See the extraction comment below.
//   * authorized_endpoints    — endpoints whose owning document is in
//                               `visible`.
//   * admitted_edges          — edges where BOTH ordinals (0 AND 1)
//                               show up in authorized_endpoints. The
//                               HAVING count clauses are the whole-
//                               relationship invariant: an edge with
//                               one visible endpoint is dropped
//                               entirely, not partially hydrated.
//
// The final SELECT hydrates edge fields + BOTH endpoint chunks' text
// and section heading in a single row per (edge, ordinal). C++ side
// groups by edge_id and rejects any edge with != 2 rows returned.
// ---------------------------------------------------------------------------
constexpr auto kGateSql = R"(
    WITH candidate_edges AS (
        SELECT DISTINCT unnest($5::uuid[]) AS edge_id
    ),
    edge_records AS (
        SELECT e.id, e.edge_type, e.direction, e.confidence,
               e.review_state, e.edge_version
        FROM   knowledge_edges e
        JOIN   candidate_edges c ON c.edge_id = e.id
        WHERE  e.company_id     = $1::uuid
          AND  e.review_state   = ANY($6::text[])
          AND  (e.expires_at    IS NULL OR e.expires_at    > now())
          AND  (e.superseded_at IS NULL OR e.superseded_at > now())
    ),
    required_endpoints AS (
        SELECT ep.edge_id, ep.ordinal, ep.chunk_id, ep.role
        FROM   knowledge_edge_endpoints ep
        JOIN   edge_records er ON er.id = ep.edge_id
        WHERE  ep.company_id = $1::uuid
    ),
    reader_scope(ou_id) AS (
        SELECT DISTINCT x FROM unnest($4::uuid[]) AS x
    ),
    reader_grant_keys AS (
        SELECT ou_id FROM reader_scope
        UNION
        SELECT c.ancestor_id
        FROM   org_unit_closure c
        WHERE  c.company_id = $1::uuid
          AND  c.descendant_id IN (SELECT ou_id FROM reader_scope)
    ),
    cand_docs AS (
        SELECT DISTINCT d.id AS doc_id,
                        d.owner_org_unit_id AS owner,
                        ep.edge_id, ep.ordinal, ep.chunk_id,
                        dc.document_version_id, dc.section_id, dc.content
        FROM   required_endpoints ep
        JOIN   document_chunks    dc ON dc.id = ep.chunk_id
        JOIN   document_versions  dv ON dv.id = dc.document_version_id
        JOIN   documents          d  ON d.id  = dv.document_id
        WHERE  dc.company_id        = $1::uuid
          AND  dv.lifecycle_status  = ANY($2::text[])
          AND  dv.sensitivity_label = ANY($3::text[])
    ),
    -- Three visibility arms — MUST stay in lock-step with EvidenceGate
    -- (G1)'s corresponding arms. Any drift between G1 and G2 is a
    -- security bug; when the shared visibility SQL is factored into a
    -- pg function (see docs §"The production query will need to
    -- reuse or factor the existing visibility SQL carefully") both
    -- gates will call it.
    visible AS (
        SELECT doc_id FROM cand_docs
        WHERE  owner IN (SELECT ou_id FROM reader_scope)
      UNION
        SELECT cd.doc_id FROM cand_docs cd
        JOIN   resource_grants rg
            ON rg.company_id    = $1::uuid
           AND rg.resource_type = 'document'
           AND rg.resource_id   = cd.doc_id
           AND rg.permission IN ('read','write','admin')
           AND (rg.expires_at IS NULL OR rg.expires_at > now())
        WHERE  (rg.principal_applies_to = 'self_only'
                AND rg.principal_id IN (SELECT ou_id FROM reader_scope))
           OR  (rg.principal_applies_to = 'self_and_descendants'
                AND rg.principal_id IN (SELECT ou_id FROM reader_grant_keys))
      UNION
        SELECT cd.doc_id FROM cand_docs cd
        JOIN   org_unit_closure rc
            ON rc.company_id    = $1::uuid
           AND rc.descendant_id = cd.owner
        JOIN   resource_grants rg
            ON rg.company_id    = $1::uuid
           AND rg.resource_type = 'org_unit'
           AND rg.resource_id   = rc.ancestor_id
           AND rg.permission IN ('read','write','admin')
           AND (rg.expires_at IS NULL OR rg.expires_at > now())
           AND (rg.resource_applies_to = 'self_and_descendants'
                OR (rg.resource_applies_to = 'self_only' AND rc.depth = 0))
        WHERE  (rg.principal_applies_to = 'self_only'
                AND rg.principal_id IN (SELECT ou_id FROM reader_scope))
           OR  (rg.principal_applies_to = 'self_and_descendants'
                AND rg.principal_id IN (SELECT ou_id FROM reader_grant_keys))
    ),
    authorized_endpoints AS (
        SELECT cd.edge_id, cd.ordinal, cd.chunk_id,
               cd.document_version_id, cd.section_id, cd.content
        FROM   cand_docs cd
        WHERE  cd.doc_id IN (SELECT doc_id FROM visible)
    ),
    admitted_edges AS (
        -- Whole-relationship invariant: an edge is admitted iff BOTH
        -- ordinals (0 AND 1) exist in authorized_endpoints. Missing or
        -- one-sided visibility drops the edge entirely.
        SELECT ep.edge_id
        FROM   required_endpoints ep
        LEFT JOIN authorized_endpoints ae
               ON ae.edge_id = ep.edge_id AND ae.ordinal = ep.ordinal
        GROUP BY ep.edge_id
        HAVING count(*) = 2
           AND count(ae.ordinal) = 2
    )
    SELECT er.id::text                     AS edge_id,
           er.edge_type,
           er.direction,
           er.confidence::float8           AS confidence,
           er.review_state,
           er.edge_version::bigint         AS edge_version,
           ae.ordinal                       AS ordinal,
           ae.chunk_id::text                AS chunk_id,
           ae.document_version_id::text    AS document_version_id,
           ae.content                       AS content,
           ds.heading                       AS section_heading
    FROM   admitted_edges a
    JOIN   edge_records            er ON er.id = a.edge_id
    JOIN   authorized_endpoints    ae ON ae.edge_id = a.edge_id
    LEFT JOIN document_sections    ds ON ds.id = ae.section_id
    ORDER  BY er.id, ae.ordinal
)";

struct HydratedRow {
    std::string  edge_id;
    std::string  edge_type;
    std::string  direction;
    double       confidence   = 0.0;
    std::string  review_state;
    std::int64_t edge_version = 0;
    int          ordinal      = 0;
    std::string  chunk_id;
    std::string  document_version_id;
    std::string  content;
    std::optional<std::string> section_heading;
};

} // namespace

drogon::Task<Result<std::vector<AllowedRelationship>>>
RelationshipEvidenceGate::evaluate(
    std::string_view                    company_id,
    const AccessScope&                  scope,
    const std::vector<std::string>&     allowed_sensitivity_labels,
    const std::vector<std::string>&     allowed_review_states,
    const std::vector<EdgeCandidate>&   candidates,
    const std::vector<std::string>&     lifecycle,
    postgres::Deadline                  deadline) const
{
    // Fail-closed short-circuits (mirror G1). Empty scope, clearance,
    // review-state whitelist, or candidate list → zero results, no DB
    // round-trip.
    if (scope.org_unit_ids.empty() || allowed_sensitivity_labels.empty()
        || allowed_review_states.empty() || candidates.empty())
        co_return std::vector<AllowedRelationship>{};

    std::vector<std::string> edge_ids;
    edge_ids.reserve(candidates.size());
    for (const auto& c : candidates)
        edge_ids.push_back(c.edge_id);

    // Materialise the (edge_id, ordinal) rows the gate returns. Group
    // by edge_id in C++ so we can reject any edge that did not come back
    // with both ordinals (should not happen given the HAVING clause,
    // but stays defense-in-depth against SQL-side drift).
    std::unordered_map<std::string, std::pair<HydratedRow, HydratedRow>> grouped;
    try {
        auto rows = co_await postgres::exec_until(
            db_, deadline, kGateSql,
            std::string(company_id),
            pg_array(lifecycle),
            pg_array(allowed_sensitivity_labels),
            pg_array(scope.org_unit_ids),
            pg_array(edge_ids),
            pg_array(allowed_review_states));
        for (const auto& r : rows) {
            HydratedRow h;
            h.edge_id             = r["edge_id"].as<std::string>();
            h.edge_type           = r["edge_type"].as<std::string>();
            h.direction           = r["direction"].as<std::string>();
            h.confidence          = r["confidence"].as<double>();
            h.review_state        = r["review_state"].as<std::string>();
            h.edge_version        = r["edge_version"].as<std::int64_t>();
            h.ordinal             = r["ordinal"].as<int>();
            h.chunk_id            = r["chunk_id"].as<std::string>();
            h.document_version_id = r["document_version_id"].as<std::string>();
            h.content             = r["content"].as<std::string>();
            if (!r["section_heading"].isNull())
                h.section_heading = r["section_heading"].as<std::string>();
            auto& slot = grouped[h.edge_id];
            if (h.ordinal == 0) slot.first  = std::move(h);
            else if (h.ordinal == 1) slot.second = std::move(h);
        }
    } catch (const drogon::orm::DrogonDbException& ex) {
        co_return std::unexpected(postgres::map_db_exception(ex));
    }

    // Emit survivors in candidate (score) order. An edge in `grouped`
    // that lacks either ordinal is treated as if it never came back —
    // whole-relationship invariant applies here too.
    std::vector<AllowedRelationship> out;
    out.reserve(grouped.size());
    for (const auto& c : candidates) {
        auto it = grouped.find(c.edge_id);
        if (it == grouped.end()) continue;
        auto& [row0, row1] = it->second;
        if (row0.chunk_id.empty() || row1.chunk_id.empty()) continue;

        AllowedRelationship::AllowedEndpoint ep0{
            .ordinal              = 0,
            .chunk_id             = std::move(row0.chunk_id),
            .document_version_id  = std::move(row0.document_version_id),
            .text                 = std::move(row0.content),
            .section_heading      = std::move(row0.section_heading),
        };
        AllowedRelationship::AllowedEndpoint ep1{
            .ordinal              = 1,
            .chunk_id             = std::move(row1.chunk_id),
            .document_version_id  = std::move(row1.document_version_id),
            .text                 = std::move(row1.content),
            .section_heading      = std::move(row1.section_heading),
        };
        out.emplace_back(AllowedRelationship::ConstructionToken{},
                         std::string(company_id),
                         std::move(row0.edge_id),   // both rows share edge_id
                         std::move(row0.edge_type),
                         std::move(row0.direction),
                         c.score,
                         row0.confidence,
                         std::move(row0.review_state),
                         row0.edge_version,
                         std::move(ep0),
                         std::move(ep1));
    }
    co_return out;
}

} // namespace wikore::rag
