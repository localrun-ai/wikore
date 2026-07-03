#include "wikore/rag/evidence_gate.hpp"
#include "wikore/adapters/postgres/error_mapper.hpp"
#include <drogon/drogon.h>
#include <string>
#include <unordered_map>

namespace wikore::rag {

namespace {

// Postgres array literal from a vector of (already-validated) strings.
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

// G1 gate query. The WITH block is the validated visibility logic (section 0,
// property-tested); the final SELECT hydrates the allowed chunks. Resource
// visibility is resolved live from resource_grants + org_unit_closure.
constexpr auto kGateSql = R"(
    WITH reader_scope(ou_id) AS (
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
        SELECT DISTINCT d.id AS doc_id, d.owner_org_unit_id AS owner
        FROM   document_chunks   dc
        JOIN   document_versions dv ON dv.id = dc.document_version_id
        JOIN   documents         d  ON d.id  = dv.document_id
        WHERE  dc.company_id        = $1::uuid
          AND  dc.id                = ANY($5::uuid[])
          AND  dv.lifecycle_status  = ANY($2::text[])
          AND  dv.sensitivity_label = ANY($3::text[])
    ),
    visible AS (
        SELECT doc_id FROM cand_docs
        WHERE  owner IN (SELECT ou_id FROM reader_scope)              -- arm 1
      UNION
        SELECT cd.doc_id FROM cand_docs cd                           -- arm 2
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
        SELECT cd.doc_id FROM cand_docs cd                           -- arm 3
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
    )
    SELECT dc.id::text                  AS chunk_id,
           dc.document_version_id::text AS document_version_id,
           dc.content                   AS content,
           ds.heading                   AS section_heading
    FROM   document_chunks   dc
    JOIN   document_versions dv ON dv.id = dc.document_version_id
    JOIN   documents         d  ON d.id  = dv.document_id
    LEFT JOIN document_sections ds ON ds.id = dc.section_id
    WHERE  dc.company_id        = $1::uuid
      AND  dc.id                = ANY($5::uuid[])
      AND  dv.lifecycle_status  = ANY($2::text[])
      AND  dv.sensitivity_label = ANY($3::text[])
      AND  d.id IN (SELECT doc_id FROM visible)
)";

struct Hydrated {
    std::string                document_version_id;   // authoritative, from PG
    std::string                content;
    std::optional<std::string> section_heading;
};

} // namespace

drogon::Task<Result<std::vector<AllowedCandidate>>>
EvidenceGate::evaluate(std::string_view                   company_id,
                       const AccessScope&                 scope,
                       const std::vector<std::string>&    allowed_sensitivity_labels,
                       const std::vector<ChunkCandidate>& candidates,
                       const std::vector<std::string>&    lifecycle,
                       postgres::Deadline                 deadline) const
{
    // Fail-closed short-circuits (also avoid a pointless round-trip): with no
    // reader scope, no clearance, or no candidates nothing can be allowed.
    if (scope.org_unit_ids.empty() || allowed_sensitivity_labels.empty()
        || candidates.empty())
        co_return std::vector<AllowedCandidate>{};

    std::vector<std::string> chunk_ids;
    chunk_ids.reserve(candidates.size());
    for (const auto& c : candidates)
        chunk_ids.push_back(c.chunk_id);

    std::unordered_map<std::string, Hydrated> allowed;
    try {
        auto rows = co_await postgres::exec_until(
            db_, deadline, kGateSql,
            std::string(company_id),
            pg_array(lifecycle),
            pg_array(allowed_sensitivity_labels),
            pg_array(scope.org_unit_ids),
            pg_array(chunk_ids));
        allowed.reserve(rows.size());
        for (const auto& r : rows) {
            Hydrated h;
            h.document_version_id = r["document_version_id"].as<std::string>();
            h.content             = r["content"].as<std::string>();
            if (!r["section_heading"].isNull())
                h.section_heading = r["section_heading"].as<std::string>();
            allowed.emplace(r["chunk_id"].as<std::string>(), std::move(h));
        }
    } catch (const drogon::orm::DrogonDbException& ex) {
        // A deadline statement_timeout (SQLSTATE 57014) maps to
        // ServiceUnavailable (503); other DB failures to database_error.
        co_return std::unexpected(postgres::map_db_exception(ex));
    }

    // Emit survivors in the candidates' (score) order; drop the rest.
    std::vector<AllowedCandidate> out;
    out.reserve(allowed.size());
    for (const auto& c : candidates) {
        auto it = allowed.find(c.chunk_id);
        if (it == allowed.end())
            continue;
        out.push_back(AllowedCandidate{
            .chunk_id            = c.chunk_id,
            // Authoritative version from PG, NOT the (possibly stale) candidate:
            // Postgres is the evidence, so evidence attribution comes from PG.
            .document_version_id = std::move(it->second.document_version_id),
            .score               = c.score,
            .text                = std::move(it->second.content),
            .section_heading     = std::move(it->second.section_heading),
        });
    }
    co_return out;
}

} // namespace wikore::rag
