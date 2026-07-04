#pragma once
#include "wikore/rag/types.hpp"
#include "wikore/domain/types.hpp"
// Result<T> and Error come from wikore/domain/types.hpp (already included via rag/types.hpp)
#include <span>
#include <string>
#include <vector>

namespace wikore::rag {

// ---------------------------------------------------------------------------
// PromptContext — the assembled prompt sent to the LLM.
// ---------------------------------------------------------------------------

struct PromptContext {
    std::string prompt;
    std::vector<std::string> source_chunk_ids; // in citation order
    int estimated_tokens = 0;                  // always <= opts.max_tokens
};

// ---------------------------------------------------------------------------
// ContextBuilderOptions — per-request knobs.
// ---------------------------------------------------------------------------

struct ContextBuilderOptions {
    // Hard token budget for the entire prompt including evidence, system
    // prompt, query, and answer headroom. Must be > 0.
    int max_tokens          = 4096;
    // Maximum evidence items to include (budget permitting). Must be >= 0.
    int max_evidence_items  = 20;
    std::string system_prompt;
};

// ---------------------------------------------------------------------------
// ContextBuilder — assembles a token-budgeted prompt from AllowedEvidence.
//
// Returns Error::invalid_input if:
//   - opts.max_tokens <= 0 or opts.max_evidence_items < 0
//   - system_prompt + query boilerplate does not fit within the budget
//     (mandatory content cannot be silently omitted)
//
// Evidence items are included in supplied order until max_tokens or
// max_evidence_items is exhausted. estimated_tokens is always <= max_tokens.
//
// Accepts only std::span<const AllowedEvidence>; not overloaded for
// ChunkCandidate, raw payloads, or diagnostic types.
// ---------------------------------------------------------------------------

class ContextBuilder {
public:
    ContextBuilder() = default;

    [[nodiscard]] Result<PromptContext> build(
        const wikore::RequestContext&    ctx,
        std::string_view                 query,
        std::span<const AllowedEvidence> evidence,
        const ContextBuilderOptions&     opts = {}) const;
};

} // namespace wikore::rag
