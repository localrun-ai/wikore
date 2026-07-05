#include "wikore/application/embed_type_vectors.hpp"
#include "wikore/rag/knowledge_edge_descriptions.hpp"
#include "wikore/adapters/postgres/error_mapper.hpp"
#include <drogon/orm/Exception.h>
#include <spdlog/spdlog.h>
#include <format>
#include <string>

namespace wikore::application {

namespace {

// Serialize a float vector as Postgres REAL[] text literal '{a,b,c}'.
// FLT_DECIMAL_DIG (9 digits) is the minimum for a float32 round-trip
// through decimal — 8 is one short (there exist float32 values that
// print-then-parse back to a ±1 ulp neighbour with 8g).
std::string to_pg_real_array(const rag::Embedding& v)
{
    std::string s = "{";
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) s += ',';
        s += std::format("{:.9g}", v[i]);
    }
    s += '}';
    return s;
}

} // namespace

drogon::Task<Result<EmbedTypeVectorsResult>>
EmbedTypeVectorsUseCase::execute(const EmbedTypeVectorsCmd& cmd)
{
    if (cmd.embedding_dimension <= 0)
        co_return std::unexpected(Error::invalid_input(
            "embedding_dimension must be positive"));
    if (embedder_->dims() != cmd.embedding_dimension)
        co_return std::unexpected(Error::invalid_state(std::format(
            "embedder dims={} does not match cmd.embedding_dimension={}",
            embedder_->dims(), cmd.embedding_dimension)));

    // Batch-embed every description in one HTTP round trip.
    std::vector<std::string> texts;
    texts.reserve(rag::kTypeDescriptions.size());
    for (const auto& [_, desc] : rag::kTypeDescriptions)
        texts.emplace_back(desc);

    auto vecs = co_await embedder_->embed_batch(texts);
    if (!vecs) co_return std::unexpected(vecs.error());
    if (vecs->size() != rag::kTypeDescriptions.size())
        co_return std::unexpected(Error::database_error(std::format(
            "embed_batch returned {} vectors, expected {}",
            vecs->size(), rag::kTypeDescriptions.size())));

    EmbedTypeVectorsResult result;
    for (size_t i = 0; i < rag::kTypeDescriptions.size(); ++i) {
        const auto& [edge_type, description] = rag::kTypeDescriptions[i];
        const auto& v                        = (*vecs)[i];
        if (static_cast<int>(v.size()) != cmd.embedding_dimension)
            co_return std::unexpected(Error::database_error(std::format(
                "embedder produced dim={} for type '{}', expected {}",
                v.size(), domain::to_wire(edge_type), cmd.embedding_dimension)));

        // UPSERT semantics:
        //   * If (formula_version, edge_type, model) does not exist → INSERT,
        //     revision defaults to 1.
        //   * If it exists → UPDATE only when the vector or description
        //     changed; bump revision by 1 in that case. Same-content
        //     re-runs are counted as unchanged so operators can see they
        //     were no-ops.
        std::string edge_type_arg{domain::to_wire(edge_type)};
        std::string desc_arg     {description};
        std::string vec_arg      = to_pg_real_array(v);
        std::string model_arg    = cmd.embedding_model_id;
        try {
            auto rows = co_await db_->execSqlCoro(
                "INSERT INTO edge_type_vectors "
                "  (formula_version, edge_type, embedding_model_id, "
                "   vector, description, revision) "
                "VALUES ($1::int, $2::text, $3::uuid, "
                "        $4::text::real[], $5::text, 1) "
                "ON CONFLICT (formula_version, edge_type, embedding_model_id) "
                "DO UPDATE SET "
                "  vector      = EXCLUDED.vector, "
                "  description = EXCLUDED.description, "
                "  revision    = edge_type_vectors.revision + 1, "
                "  embedded_at = now() "
                "WHERE edge_type_vectors.vector      IS DISTINCT FROM EXCLUDED.vector "
                "   OR edge_type_vectors.description IS DISTINCT FROM EXCLUDED.description "
                "RETURNING revision",
                rag::kFormulaVersion,
                edge_type_arg,
                model_arg,
                vec_arg,
                desc_arg);
            if (rows.empty()) ++result.rows_unchanged;
            else               ++result.rows_upserted;
        } catch (const drogon::orm::DrogonDbException& ex) {
            co_return std::unexpected(postgres::map_db_exception(ex));
        }
    }
    spdlog::info("[embed-type-vectors] model={} formula_version={} "
                 "upserted={} unchanged={}",
                 cmd.embedding_model_id, rag::kFormulaVersion,
                 result.rows_upserted, result.rows_unchanged);
    co_return result;
}

} // namespace wikore::application
