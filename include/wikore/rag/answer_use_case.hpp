#pragma once

#include "wikore/rag/context_builder.hpp"
#include "wikore/rag/llm_provider.hpp"
#include "wikore/rag/retrieval_orchestrator.hpp"
#include "wikore/rag/types.hpp"
#include "wikore/domain/types.hpp"

#include <drogon/utils/coroutine.h>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace wikore::rag {

// ---------------------------------------------------------------------------
// AnswerUseCase — the composition step that turns a retrieval intent + query
// into a grounded LLM answer with citations.
//
// Pipeline:
//   1. RetrievalOrchestrator::retrieve_evidence(intent, bridge_opts)
//      →  gated AllowedEvidence variants (chunks + relationships).
//   2. ContextBuilder::build
//      →  a PromptContext with [SRC N] blocks for chunks and [REL N]
//         blocks for relationships (per docs §"Context construction").
//         Endpoint text is emitted once even when cited by multiple
//         relationships; source_chunk_ids and source_edge_ids record
//         the citation order.
//   3. LlmProviderPort::chat
//      →  the LLM generates an answer over the prompt; the system
//         message includes the ANSWER-SEMANTICS instructions (see
//         answer_use_case.cpp:kAnswerSystemPrompt) describing what
//         each [REL N] origin means so the model does not claim an
//         llm_proposal edge is a confirmed fact.
//
// Deadline discipline: the ctx.deadline flows through all three steps.
// Retrieval uses the shared budget internally; the LLM call takes the
// remaining time as its per-call timeout so a hung upstream cannot
// exceed the request's whole-call SLO.
//
// Failure model: every step returns Result<T>; the pipeline stops at
// the first failure and propagates its Error. Bridge intent without a
// wired edge collection surfaces here as unavailable, exactly as it
// does at /wiki/evidence (fail closed rather than silently degrade).
// ---------------------------------------------------------------------------

struct AnswerOptions {
    // Which retrieval surface to run. Same values as /wiki/evidence.
    RetrievalIntent      intent      = RetrievalIntent::Automatic;
    // Bridge/Automatic tuning. Ignored by Fact intent.
    BridgeIntentOptions  bridge_opts = {};
    // Cap on retrieved evidence items handed to the context builder.
    // Distinct from context_opts.max_evidence_items (which is a
    // secondary cap applied AFTER retrieval); this one bounds the
    // Qdrant fanout.
    int                  limit       = 10;
    // Prompt shape and budgets. If the caller does not set
    // system_prompt, the use case fills in kAnswerSystemPrompt so
    // the LLM sees the answer-semantics rules.
    ContextBuilderOptions context_opts = {};
    // LLM per-call timeout in seconds. Zero uses the provider's
    // default cap. Callers typically pass the remaining request
    // deadline (see run() for the standard pattern).
    double               llm_timeout_s = 0.0;
    // Model override. Empty = use the provider's default model.
    std::string          model         = {};
};

struct AnswerResult {
    // The LLM's generated answer text. Not post-processed here; the
    // AnswerFinalizer citation-validation step (docs §"AnswerFinalizer
    // with relationship citations") is a follow-up.
    std::string              answer;
    // Citation IDs the prompt exposed to the model, in [SRC N] and
    // [REL N] order. A caller-side citation-validation pass should
    // treat citations outside these sets as hallucinations.
    std::vector<std::string> source_chunk_ids;
    std::vector<std::string> source_edge_ids;
    // Telemetry for logging / cost accounting.
    std::string              model;
    int                      input_tokens  = 0;
    int                      output_tokens = 0;
    // Number of evidence items that reached the prompt (chunks + rels
    // AFTER context builder truncation). Useful for observability of
    // "did we ground on anything?" without exposing the citation IDs.
    int                      evidence_included = 0;
};

class AnswerUseCase {
public:
    AnswerUseCase(std::shared_ptr<RetrievalOrchestrator> orch,
                  std::shared_ptr<ContextBuilder>        ctx_builder,
                  std::shared_ptr<LlmProviderPort>       llm);

    // Runs the pipeline. Every error kind maps cleanly to an HTTP
    // status via the same status_for helper the handlers use:
    //   InvalidInput       → 400
    //   Forbidden          → 403  (not currently emitted here, but
    //                               reserved for AnswerFinalizer)
    //   NotFound           → 404
    //   ServiceUnavailable → 503  (LLM upstream failure / hung)
    //   InvalidState       → 500
    //   DatabaseError      → 500
    drogon::Task<Result<AnswerResult>>
    run(const RequestContext&      ctx,
        std::string                query,
        std::string_view           scope_org_unit_id,
        const AnswerOptions&       opts) const;

private:
    std::shared_ptr<RetrievalOrchestrator> orch_;
    std::shared_ptr<ContextBuilder>        ctx_builder_;
    std::shared_ptr<LlmProviderPort>       llm_;
};

// The default system prompt the use case supplies when the caller does
// not override context_opts.system_prompt. Kept as a header-visible
// constant so tests can pin its content (breaks if the answer-semantics
// contract changes without a test update).
extern const char* const kAnswerSystemPrompt;

} // namespace wikore::rag
