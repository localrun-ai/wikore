// ---------------------------------------------------------------------------
// ContextBuilder — Iteration 3 implementation.
//
// Assembles a token-budgeted prompt from AllowedEvidence. Evidence items are
// included in the order supplied until either max_evidence_items or the token
// budget (max_tokens minus headroom for the answer) is exhausted.
//
// Token estimation: 4 chars ≈ 1 token (standard rough heuristic).  The
// caller must size max_tokens conservatively; ContextBuilder never exceeds it.
// ---------------------------------------------------------------------------

#include "wikore/rag/context_builder.hpp"
#include <format>
#include <spdlog/spdlog.h>

namespace wikore::rag {

// Reserve this many tokens for the model's answer within max_tokens.
static constexpr int kAnswerHeadroomTokens = 512;

// Estimate tokens from a string (4 chars ≈ 1 token).
static int estimate_tokens(std::string_view s) noexcept
{
    return std::max(1, static_cast<int>(s.size() / 4));
}

PromptContext ContextBuilder::build(
    const wikore::RequestContext& /*ctx*/,
    std::string_view               query,
    std::span<const AllowedEvidence> evidence,
    const ContextBuilderOptions&   opts) const
{
    // Compute the token budget available for evidence + boilerplate.
    // Reserve kAnswerHeadroomTokens for the model's response.
    const int answer_headroom = std::min(kAnswerHeadroomTokens, opts.max_tokens / 4);
    int remaining_tokens = opts.max_tokens - answer_headroom;

    PromptContext out;
    out.prompt.reserve(opts.max_tokens * 4); // pre-alloc rough upper bound

    // System prompt — charged against the budget first.
    if (!opts.system_prompt.empty()) {
        const auto sys_tokens = estimate_tokens(opts.system_prompt) + 1 /*\n\n*/;
        remaining_tokens -= sys_tokens;
        if (remaining_tokens <= 0) {
            spdlog::warn("[context-builder] system prompt alone exceeds token budget");
            remaining_tokens = 0;
        } else {
            out.prompt += opts.system_prompt;
            out.prompt += "\n\n";
        }
    }

    // Query boilerplate — "Question: ...\n\nAnswer:" charged upfront so we
    // never emit evidence that would push the query itself over budget.
    const std::string query_block = std::format("Question: {}\n\nAnswer:", query);
    remaining_tokens -= estimate_tokens(query_block);

    // Assemble evidence blocks within budget.
    int n = 0;
    int truncated = 0;
    for (const auto& ev : evidence) {
        if (n >= opts.max_evidence_items) { ++truncated; continue; }
        std::visit([&](const AllowedChunk& chunk) {
            // Estimate cost: header "[SRC N]\n" ≈ 3 tokens + text.
            const int cost = 3 + estimate_tokens(chunk.text);
            if (cost > remaining_tokens) { ++truncated; return; }
            remaining_tokens -= cost;
            out.prompt += std::format("[SRC {}]\n{}\n\n", ++n, chunk.text);
            out.source_chunk_ids.push_back(chunk.chunk_id);
        }, ev);
    }

    out.prompt += query_block;
    out.estimated_tokens = opts.max_tokens - remaining_tokens;

    if (truncated > 0)
        spdlog::debug("[context-builder] {} evidence items included, {} truncated by budget",
                      n, truncated);
    else
        spdlog::debug("[context-builder] {} evidence items, ~{} tokens",
                      n, out.estimated_tokens);
    return out;
}

} // namespace wikore::rag
