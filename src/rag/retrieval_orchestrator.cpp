#include "wikore/rag/retrieval_orchestrator.hpp"
#include "wikore/rag/qdrant_filter_builder.hpp"
#include "wikore/rag/clearance.hpp"
#include <algorithm>
#include <chrono>
#include <utility>

namespace wikore::rag {

drogon::Task<Result<std::vector<AllowedCandidate>>>
RetrievalOrchestrator::retrieve(const RequestContext& ctx,
                                std::string           query,
                                std::string_view      scope_org_unit_id,
                                int                   limit) const
{
    if (ctx.deadline_exceeded())
        co_return std::unexpected(Error::unavailable("retrieve: deadline exceeded"));

    // Validate before doing any work: a non-positive limit would flow into
    // resize(limit), where a negative int converts to a huge size_t.
    if (limit <= 0)
        co_return std::unexpected(Error::invalid_input("retrieve: limit must be positive"));

    const auto& company = ctx.tenant.company_id;

    // Seconds left on the request deadline. Passed as the per-call timeout to
    // the embed and Qdrant HTTP steps so their budgets come from the SAME
    // shared deadline rather than independent fixed caps (which could sum past
    // it, e.g. a 20s embed then a 30s search). Combined with the between-step
    // deadline checks, this bounds total request time. DB steps (resolve, gate,
    // tenant lookup) are separately bounded by the DbClient's configured query
    // timeout.
    auto remaining_s = [&] {
        const auto rem = ctx.deadline - std::chrono::steady_clock::now();
        return std::max(0.0, std::chrono::duration<double>(rem).count());
    };

    // 1. embed the query.
    auto vec = co_await embedder_->embed(std::move(query), remaining_s());
    if (!vec) co_return std::unexpected(vec.error());

    // 2. resolve the reader's scope (cached; epoch-validated).
    if (ctx.deadline_exceeded())
        co_return std::unexpected(Error::unavailable("retrieve: deadline exceeded after embed"));
    auto scope = co_await resolver_->resolve(
        company, ctx.principal.user_id, scope_org_unit_id);
    if (!scope) co_return std::unexpected(scope.error());

    // 3. derive clearance - the single enforced sensitivity policy.
    const auto labels = allowed_labels_for(ctx.principal);

    // 4. build the prefilter (fail-closed on empty scope/clearance).
    const auto filter = QdrantFilterBuilder::build(company, *scope, labels);

    // 5. vector search, over-fetched so gate drops do not starve the result.
    //    Computed in 64-bit and capped so `limit * over_fetch` cannot overflow
    //    int for a large caller-supplied limit (limit is already > 0).
    constexpr long long kMaxFetch = 10000;
    const long long want  = static_cast<long long>(limit)
                          * static_cast<long long>(std::max(1, over_fetch_));
    const int fetch = static_cast<int>(std::min(want, kMaxFetch));
    if (ctx.deadline_exceeded())
        co_return std::unexpected(Error::unavailable("retrieve: deadline exceeded before search"));
    auto candidates = co_await vector_store_->search(*vec, filter, fetch, remaining_s());
    if (!candidates) co_return std::unexpected(candidates.error());

    // 6. EvidenceGate: authoritative live re-validation + hydration. Same
    //    scope and clearance as the prefilter, so the two layers agree.
    if (ctx.deadline_exceeded())
        co_return std::unexpected(Error::unavailable("retrieve: deadline exceeded before gate"));
    auto allowed = co_await gate_.evaluate(company, *scope, labels, *candidates);
    if (!allowed) co_return std::unexpected(allowed.error());

    // Return up to `limit`, in the gate-preserved (retrieval score) order.
    if (static_cast<int>(allowed->size()) > limit)
        allowed->resize(limit);
    co_return std::move(*allowed);
}

} // namespace wikore::rag
