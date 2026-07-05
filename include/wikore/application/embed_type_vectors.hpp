#pragma once
#include "wikore/domain/types.hpp"
#include "wikore/rag/embedder.hpp"
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <memory>
#include <string>

namespace wikore::application {

// ---------------------------------------------------------------------------
// EmbedTypeVectorsUseCase — admin one-shot job.
//
// For each entry in wikore::rag::kTypeDescriptions:
//   1. Embed the description via the provided EmbedderPort.
//   2. UPSERT the resulting vector into edge_type_vectors keyed by
//      (formula_version, edge_type, embedding_model_id).
//
// Called from an admin CLI or a boot task. Idempotent: re-running the
// job with the SAME kTypeDescriptions and SAME embedding model produces
// the same rows (bumps `revision` on each UPSERT so operator diffs are
// visible). If a description string changed WITHOUT bumping
// kFormulaVersion, the corresponding row is silently overwritten — that
// is intentional: the invariant is 'never change a description without
// bumping the formula version', and violating it is a caller-side bug
// this job does not try to detect.
//
// Callers pass the embedding model row directly (id + dimension) so the
// use case does not hard-code a lookup by name.
// ---------------------------------------------------------------------------

struct EmbedTypeVectorsCmd {
    Uuid embedding_model_id;   // FK to embedding_models.id
    int  embedding_dimension;  // must match the model's registered dim
};

struct EmbedTypeVectorsResult {
    int rows_upserted   = 0;
    int rows_unchanged  = 0;   // description text was already current
};

class EmbedTypeVectorsUseCase {
public:
    EmbedTypeVectorsUseCase(drogon::orm::DbClientPtr           db,
                            std::shared_ptr<rag::EmbedderPort> embedder)
        : db_(std::move(db)), embedder_(std::move(embedder)) {}

    drogon::Task<Result<EmbedTypeVectorsResult>>
    execute(const EmbedTypeVectorsCmd& cmd);

private:
    drogon::orm::DbClientPtr           db_;
    std::shared_ptr<rag::EmbedderPort> embedder_;
};

} // namespace wikore::application
