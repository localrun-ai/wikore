#include "wikore/rag/retrieval_orchestrator.hpp"
#include "wikore/rag/qdrant_filter_builder.hpp"
#include "wikore/rag/clearance.hpp"
#include <algorithm>
#include <chrono>
#include <utility>

namespace wikore::rag {

namespace {

// Seconds left on the shared request deadline; used as the per-call
// timeout for the embed and Qdrant HTTP steps so their budgets come
// from the SAME deadline rather than independent fixed caps.
double remaining_s(const RequestContext& ctx)
{
    const auto rem = ctx.deadline - std::chrono::steady_clock::now();
    return std::max(0.0, std::chrono::duration<double>(rem).count());
}

// over_fetch multiplied by limit, capped at a sanity ceiling. Same
// formula on both sides so a symmetric Automatic request pulls
// symmetric candidate pools before per-side truncation.
int compute_fetch(int limit, int over_fetch)
{
    constexpr long long kMaxFetch = 10000;
    const long long want  = static_cast<long long>(limit)
                          * static_cast<long long>(std::max(1, over_fetch));
    return static_cast<int>(std::min(want, kMaxFetch));
}

// Chunk search + G1 gate. Both entry points (retrieve() and
// retrieve_evidence()) hoist embed+resolve into their outer coroutine
// and pass the pre-computed Embedding and AccessScope in — a query
// under Automatic intent embeds and resolves exactly ONCE, not twice.
// Returns the AllowedChunk list in score order, truncated to `limit`.
drogon::Task<Result<std::vector<AllowedChunk>>>
retrieve_chunks(const RequestContext&                       ctx,
                const Embedding&                            vec,
                const AccessScope&                          scope,
                const std::vector<std::string>&             labels,
                int                                         limit,
                int                                         over_fetch,
                const std::shared_ptr<VectorStorePort>&     chunk_store,
                const EvidenceGate&                         gate)
{
    const auto& company = ctx.tenant.company_id;
    const auto filter = QdrantFilterBuilder::build(company, scope, labels);
    const int fetch = compute_fetch(limit, over_fetch);

    if (ctx.deadline_exceeded())
        co_return std::unexpected(Error::unavailable("retrieve: deadline exceeded before search"));
    auto candidates = co_await chunk_store->search(vec, filter, fetch, remaining_s(ctx));
    if (!candidates) co_return std::unexpected(candidates.error());

    if (ctx.deadline_exceeded())
        co_return std::unexpected(Error::unavailable("retrieve: deadline exceeded before gate"));
    auto allowed = co_await gate.evaluate(
        company, scope, labels, *candidates, {"active"}, ctx.deadline);
    if (!allowed) co_return std::unexpected(allowed.error());

    if (static_cast<int>(allowed->size()) > limit)
        allowed->erase(allowed->begin() + limit, allowed->end());
    co_return std::move(*allowed);
}

// Edge search + G2 gate. Same rationale as retrieve_chunks: embed and
// resolve are computed once in the caller. Returns AllowedRelationship
// values in candidate (score) order, truncated to `limit`.
drogon::Task<Result<std::vector<AllowedRelationship>>>
retrieve_edges(const RequestContext&                                ctx,
               const Embedding&                                     vec,
               const AccessScope&                                   scope,
               const std::vector<std::string>&                      labels,
               int                                                  limit,
               int                                                  over_fetch,
               const BridgeIntentOptions&                           bridge_opts,
               const std::shared_ptr<RelationshipVectorStorePort>&  edge_store,
               const std::shared_ptr<RelationshipEvidenceGate>&     edge_gate)
{
    const auto& company = ctx.tenant.company_id;
    const int fetch = compute_fetch(limit, over_fetch);

    if (ctx.deadline_exceeded())
        co_return std::unexpected(Error::unavailable("retrieve: deadline exceeded before edge search"));
    auto candidates = co_await edge_store->search(
        company, vec, bridge_opts.allowed_review_states,
        bridge_opts.min_confidence, fetch, remaining_s(ctx));
    if (!candidates) co_return std::unexpected(candidates.error());

    if (ctx.deadline_exceeded())
        co_return std::unexpected(Error::unavailable("retrieve: deadline exceeded before edge gate"));
    auto allowed = co_await edge_gate->evaluate(
        company, scope, labels, bridge_opts.allowed_review_states,
        *candidates, {"active"}, ctx.deadline);
    if (!allowed) co_return std::unexpected(allowed.error());

    if (static_cast<int>(allowed->size()) > limit)
        allowed->erase(allowed->begin() + limit, allowed->end());
    co_return std::move(*allowed);
}

} // namespace

drogon::Task<Result<std::vector<AllowedChunk>>>
RetrievalOrchestrator::retrieve(const RequestContext& ctx,
                                std::string           query,
                                std::string_view      scope_org_unit_id,
                                int                   limit) const
{
    // Backward-compat entry point. Embed + resolve happen here, then
    // delegate to the shared chunk helper.
    if (ctx.deadline_exceeded())
        co_return std::unexpected(Error::unavailable("retrieve: deadline exceeded"));
    if (limit <= 0)
        co_return std::unexpected(Error::invalid_input("retrieve: limit must be positive"));

    const auto& company = ctx.tenant.company_id;

    auto vec = co_await embedder_->embed(query, remaining_s(ctx));
    if (!vec) co_return std::unexpected(vec.error());

    if (ctx.deadline_exceeded())
        co_return std::unexpected(Error::unavailable("retrieve: deadline exceeded after embed"));
    auto scope = co_await resolver_->resolve(
        company, ctx.principal.user_id, scope_org_unit_id, ctx.deadline);
    if (!scope) co_return std::unexpected(scope.error());

    const auto labels = allowed_labels_for(ctx.principal);
    co_return co_await retrieve_chunks(
        ctx, *vec, *scope, labels, limit, over_fetch_,
        chunk_store_, chunk_gate_);
}

drogon::Task<Result<std::vector<AllowedEvidence>>>
RetrievalOrchestrator::retrieve_evidence(const RequestContext&      ctx,
                                         std::string                query,
                                         std::string_view           scope_org_unit_id,
                                         RetrievalIntent            intent,
                                         const BridgeIntentOptions& bridge_opts,
                                         int                        limit) const
{
    if (ctx.deadline_exceeded())
        co_return std::unexpected(Error::unavailable("retrieve: deadline exceeded"));
    if (limit <= 0)
        co_return std::unexpected(Error::invalid_input("retrieve: limit must be positive"));

    // Bridge intent is the explicit request and MUST fail closed when
    // the edge store / gate is not configured. Automatic below tolerates
    // the same condition by degrading to fact-only.
    const bool have_edges = (edge_store_ != nullptr && edge_gate_ != nullptr);
    if (intent == RetrievalIntent::Bridge && !have_edges)
        co_return std::unexpected(Error::unavailable(
            "retrieve: bridge intent requested but no edge vector store / gate configured"));

    // Non-empty review_state list is required for any intent that
    // actually hits the edge path (bridge and automatic-with-edges).
    // Empty would silently match zero — fail loud instead.
    const bool will_run_edges =
        (intent == RetrievalIntent::Bridge)
        || (intent == RetrievalIntent::Automatic && have_edges);
    if (will_run_edges && bridge_opts.allowed_review_states.empty())
        co_return std::unexpected(Error::invalid_input(
            "retrieve: bridge/automatic intent requires at least one allowed review_state"));

    const auto& company = ctx.tenant.company_id;

    // Embed + resolve exactly ONCE per request, regardless of intent.
    // Under Automatic the same Embedding and AccessScope feed both
    // sub-searches — no double network round trip.
    auto vec = co_await embedder_->embed(query, remaining_s(ctx));
    if (!vec) co_return std::unexpected(vec.error());

    if (ctx.deadline_exceeded())
        co_return std::unexpected(Error::unavailable("retrieve: deadline exceeded after embed"));
    auto scope = co_await resolver_->resolve(
        company, ctx.principal.user_id, scope_org_unit_id, ctx.deadline);
    if (!scope) co_return std::unexpected(scope.error());

    const auto labels = allowed_labels_for(ctx.principal);

    switch (intent) {
    case RetrievalIntent::Fact: {
        auto r = co_await retrieve_chunks(
            ctx, *vec, *scope, labels, limit, over_fetch_,
            chunk_store_, chunk_gate_);
        if (!r) co_return std::unexpected(r.error());
        std::vector<AllowedEvidence> out;
        out.reserve(r->size());
        for (auto& c : *r) out.emplace_back(std::move(c));
        co_return out;
    }
    case RetrievalIntent::Bridge: {
        auto r = co_await retrieve_edges(
            ctx, *vec, *scope, labels, limit, over_fetch_, bridge_opts,
            edge_store_, edge_gate_);
        if (!r) co_return std::unexpected(r.error());
        std::vector<AllowedEvidence> out;
        out.reserve(r->size());
        for (auto& rel : *r) out.emplace_back(std::move(rel));
        co_return out;
    }
    case RetrievalIntent::Automatic: {
        // Both sources at once, sharing the same embedding and scope.
        // Each side runs its own gate and returns its own truncated
        // result; we merge by interleaving (chunk, edge, chunk, edge,
        // ...) up to `limit` total.
        //
        // Interleave is deliberately simple: it preserves per-kind
        // score order and gives edges representation in the top of
        // the mixed list without requiring a calibrated cross-kind
        // scale. A future combined reranker will replace this with a
        // proper normalization (design doc §"Score normalization"
        // lists the axes: vector similarity, relationship
        // confidence, endpoint authority, edge origin, review state,
        // path length, endpoint diversity, source freshness,
        // fact-vs-inferred coverage). Documented intentionally naive
        // here so the choice does not read as a scoring bug.
        auto chunks_r = co_await retrieve_chunks(
            ctx, *vec, *scope, labels, limit, over_fetch_,
            chunk_store_, chunk_gate_);
        if (!chunks_r) co_return std::unexpected(chunks_r.error());

        // Automatic gracefully degrades to fact-only when the edge
        // store / gate is not configured — the caller asked for
        // 'either', so bridge unavailability is a partial answer, not
        // a failure.
        std::vector<AllowedRelationship> edges;
        if (have_edges) {
            auto edges_r = co_await retrieve_edges(
                ctx, *vec, *scope, labels, limit, over_fetch_, bridge_opts,
                edge_store_, edge_gate_);
            if (!edges_r) co_return std::unexpected(edges_r.error());
            edges = std::move(*edges_r);
        }

        std::vector<AllowedEvidence> out;
        out.reserve(std::min<int>(limit,
            static_cast<int>(chunks_r->size() + edges.size())));
        std::size_t ci = 0, ei = 0;
        while (static_cast<int>(out.size()) < limit
               && (ci < chunks_r->size() || ei < edges.size())) {
            if (ci < chunks_r->size())
                out.emplace_back(std::move((*chunks_r)[ci++]));
            if (static_cast<int>(out.size()) >= limit) break;
            if (ei < edges.size())
                out.emplace_back(std::move(edges[ei++]));
        }
        co_return out;
    }
    }
    // enum is exhaustive; keep the compiler happy on -Wswitch.
    co_return std::vector<AllowedEvidence>{};
}

} // namespace wikore::rag
