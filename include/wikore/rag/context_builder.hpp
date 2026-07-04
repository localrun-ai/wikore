#pragma once
#include "wikore/rag/types.hpp"
#include "wikore/domain/types.hpp"
// Result<T> and Error come from wikore/domain/types.hpp (already via rag/types.hpp)
#include <span>
#include <string>
#include <vector>

namespace wikore::rag {

// ---------------------------------------------------------------------------
// PromptContext — the assembled prompt sent to the LLM.
// ---------------------------------------------------------------------------

struct PromptContext {
    std::string prompt;
    std::vector<std::string> source_chunk_ids; // in citation order, gated tenant
    // Bytes used by the assembled prompt. Always <= opts.max_prompt_bytes.
    // Note: this is a byte count, not a model-token count. True token counts
    // depend on the tokenizer; use this only as a conservative admission guard.
    std::size_t prompt_bytes = 0;
};

// ---------------------------------------------------------------------------
// ContextBuilderOptions — per-request knobs.
// ---------------------------------------------------------------------------

static constexpr std::size_t kContextBuilderMaxBytes = 4UL * 1024UL * 1024UL; // 4 MiB hard cap

struct ContextBuilderOptions {
    // Byte budget for the assembled prompt (UTF-8 bytes, not model tokens).
    // This is a conservative upper bound: most tokenizers produce 1 token per
    // 3–6 bytes of English text, so a byte budget is safe for admission even
    // when the exact token count is unknown.
    // Must be in [1, kContextBuilderMaxBytes]. Default matches ~4k tokens of
    // English text at 4 bytes/token.
    std::size_t max_prompt_bytes  = 16UL * 1024;  // 16 KiB ≈ 4k tokens
    // Maximum evidence items (budget permitting). Must be >= 0.
    int         max_evidence_items = 20;
    std::string system_prompt;
};

// ---------------------------------------------------------------------------
// ContextBuilder — assembles a byte-budgeted prompt from AllowedEvidence.
//
// Returns Error::invalid_input if:
//   - opts.max_prompt_bytes is 0 or > kContextBuilderMaxBytes
//   - opts.max_evidence_items < 0
//   - system_prompt + query boilerplate do not fit within the budget
//   - any evidence item's company_id does not match ctx.tenant.company_id
//     (cross-tenant evidence is a logic error, returned as internal error)
//
// Evidence items are included in supplied order until max_prompt_bytes or
// max_evidence_items is exhausted. prompt_bytes is always <= max_prompt_bytes.
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
