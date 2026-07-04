#include "wikore/rag/context_builder.hpp"
#include <format>
#include <spdlog/spdlog.h>

namespace wikore::rag {

static constexpr int kAnswerHeadroomFraction = 4; // reserve max_tokens/4 for answer
static constexpr int kMinAnswerHeadroom      = 64;
static constexpr int kMaxAnswerHeadroom      = 512;

static int estimate_tokens(std::string_view s) noexcept {
    return std::max(1, static_cast<int>(s.size() / 4));
}

Result<PromptContext> ContextBuilder::build(
    const wikore::RequestContext& /*ctx*/,
    std::string_view               query,
    std::span<const AllowedEvidence> evidence,
    const ContextBuilderOptions&   opts) const
{
    // -----------------------------------------------------------------------
    // 1. Validate options.
    // -----------------------------------------------------------------------
    if (opts.max_tokens <= 0)
        return std::unexpected(Error::invalid_input(
            "context_builder: max_tokens must be > 0"));
    if (opts.max_evidence_items < 0)
        return std::unexpected(Error::invalid_input(
            "context_builder: max_evidence_items must be >= 0"));

    // -----------------------------------------------------------------------
    // 2. Reserve answer headroom, then check mandatory content fits.
    // -----------------------------------------------------------------------
    const int answer_headroom = std::clamp(
        opts.max_tokens / kAnswerHeadroomFraction,
        kMinAnswerHeadroom, kMaxAnswerHeadroom);

    int remaining = opts.max_tokens - answer_headroom;

    // System prompt (optional but, if provided, mandatory to include in full).
    if (!opts.system_prompt.empty()) {
        const int sys_cost = estimate_tokens(opts.system_prompt) + 1; // +1 for \n\n
        if (sys_cost > remaining)
            return std::unexpected(Error::invalid_input(
                "context_builder: system prompt exceeds token budget"));
        remaining -= sys_cost;
    }

    // Query boilerplate is always mandatory.
    const std::string query_block =
        std::format("Question: {}\n\nAnswer:", query);
    const int query_cost = estimate_tokens(query_block);
    if (query_cost > remaining)
        return std::unexpected(Error::invalid_input(
            "context_builder: query exceeds remaining token budget "
            "(increase max_tokens or shorten the query)"));
    remaining -= query_cost;

    // -----------------------------------------------------------------------
    // 3. Build prompt.
    // -----------------------------------------------------------------------
    PromptContext out;
    out.prompt.reserve(static_cast<std::size_t>(opts.max_tokens) * 4);

    if (!opts.system_prompt.empty()) {
        out.prompt += opts.system_prompt;
        out.prompt += "\n\n";
    }

    int n = 0, truncated = 0;
    for (const auto& ev : evidence) {
        if (n >= opts.max_evidence_items) { ++truncated; continue; }
        std::visit([&](const AllowedChunk& chunk) {
            const int cost = 3 + estimate_tokens(chunk.text());
            if (cost > remaining) { ++truncated; return; }
            remaining -= cost;
            out.prompt += std::format("[SRC {}]\n{}\n\n", ++n, chunk.text());
            out.source_chunk_ids.push_back(chunk.chunk_id());
        }, ev);
    }

    out.prompt += query_block;
    out.estimated_tokens = opts.max_tokens - answer_headroom - remaining;

    if (truncated > 0)
        spdlog::debug("[context-builder] {}/{} items included, {} truncated by budget",
                      n, static_cast<int>(evidence.size()), truncated);
    else
        spdlog::debug("[context-builder] {} items, ~{} tokens",
                      n, out.estimated_tokens);
    return out;
}

} // namespace wikore::rag
