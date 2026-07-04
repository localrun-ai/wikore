// ---------------------------------------------------------------------------
// ContextBuilder — stub implementation (Iteration 3).
//
// The full implementation arrives with AnswerFinalizer; this stub satisfies
// the AllowedEvidence type boundary and allows chat endpoint development to
// proceed without a complete prompt-assembly pipeline.
// ---------------------------------------------------------------------------

#include "wikore/rag/context_builder.hpp"
#include <format>
#include <spdlog/spdlog.h>

namespace wikore::rag {

PromptContext ContextBuilder::build(
    const wikore::RequestContext& /*ctx*/,
    std::string_view               query,
    std::span<const AllowedEvidence> evidence,
    const ContextBuilderOptions&   opts) const
{
    PromptContext out;
    out.prompt.reserve(1024);

    if (!opts.system_prompt.empty()) {
        out.prompt += opts.system_prompt;
        out.prompt += "\n\n";
    }

    // Assemble evidence blocks, visiting each AllowedEvidence variant.
    int n = 0;
    for (const auto& ev : evidence) {
        if (n >= opts.max_evidence_items) break;
        std::visit([&](const AllowedChunk& chunk) {
            out.prompt += std::format("[SRC {}]\n{}\n\n", ++n, chunk.text);
            out.source_chunk_ids.push_back(chunk.chunk_id);
        }, ev);
    }

    out.prompt += "Question: ";
    out.prompt += query;
    out.prompt += "\n\nAnswer:";

    // Rough token estimate: 4 chars ≈ 1 token.
    out.estimated_tokens = static_cast<int>(out.prompt.size() / 4);

    spdlog::debug("[context-builder] {} evidence items, ~{} tokens",
                  n, out.estimated_tokens);
    return out;
}

} // namespace wikore::rag
