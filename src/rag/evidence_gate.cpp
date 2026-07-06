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

// G1 gate query. The visibility arms live in the shared V038 function
// wikore_visible_doc_ids so this file and RelationshipEvidenceGate
// (G2) cannot drift — see V038 header for the arm-by-arm contract.
constexpr auto kGateSql = R"(
    WITH cand_docs AS (
        SELECT DISTINCT d.id AS doc_id
        FROM   document_chunks   dc
        JOIN   document_versions dv ON dv.id = dc.document_version_id
        JOIN   documents         d  ON d.id  = dv.document_id
        WHERE  dc.company_id        = $1::uuid
          AND  dc.id                = ANY($5::uuid[])
          AND  dv.lifecycle_status  = ANY($2::text[])
          AND  dv.sensitivity_label = ANY($3::text[])
    ),
    visible AS (
        SELECT doc_id
        FROM   wikore_visible_doc_ids(
                   $1::uuid,
                   $4::uuid[],
                   (SELECT array_agg(doc_id) FROM cand_docs))
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

drogon::Task<Result<std::vector<AllowedChunk>>>
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
        co_return std::vector<AllowedChunk>{};

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
    std::vector<AllowedChunk> out;
    out.reserve(allowed.size());
    for (const auto& c : candidates) {
        auto it = allowed.find(c.chunk_id);
        if (it == allowed.end())
            continue;
        out.push_back(AllowedChunk{
            AllowedChunk::ConstructionToken{},
            std::string(company_id),           // tenant binding stamped here
            c.chunk_id,
            // Authoritative version from PG, NOT the (possibly stale) candidate:
            std::move(it->second.document_version_id),
            c.score,
            std::move(it->second.content),
            std::move(it->second.section_heading),
        });
    }
    co_return out;
}

} // namespace wikore::rag
