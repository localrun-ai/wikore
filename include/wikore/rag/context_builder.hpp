#pragma once
#include "wikore/rag/types.hpp"
#include "wikore/domain/types.hpp"
#include <span>
#include <string>
#include <vector>

namespace wikore::rag {

// ---------------------------------------------------------------------------
// PromptContext — the assembled prompt sent to the LLM.
//
// Carries the formatted prompt string, the ordered list of source citations
// (for AnswerFinalizer), and the total token estimate used for budget enforcement.
// ---------------------------------------------------------------------------

struct PromptContext {
    std::string prompt;                        // assembled prompt text
    std::vector<std::string> source_chunk_ids; // in citation order
    int estimated_tokens = 0;
};

// ---------------------------------------------------------------------------
// ContextBuilderOptions — per-request knobs.
// ---------------------------------------------------------------------------

struct ContextBuilderOptions {
    int max_tokens          = 4096;  // hard token budget for the prompt
    int max_evidence_items  = 20;    // cap on evidence items to include
    std::string system_prompt;       // injected before evidence
};

// ---------------------------------------------------------------------------
// ContextBuilder — assembles a PromptContext from allowed evidence.
//
// The function signature is intentionally strict:
//   - Accepts std::span<const AllowedEvidence> only.
//   - Must NOT be overloaded for ChunkCandidate, EdgeCandidate, raw Qdrant
//     payloads, arbitrary chunk IDs, or RetrievalDiagnostics.
//   - AllowedEvidence is a variant<AllowedChunk> today; adding
//     AllowedRelationship/AllowedPath later does not require a new overload.
//
// Thread safety: ContextBuilder is stateless.
// ---------------------------------------------------------------------------

class ContextBuilder {
public:
    ContextBuilder() = default;

    [[nodiscard]] PromptContext build(
        const wikore::RequestContext&  ctx,
        std::string_view               query,
        std::span<const AllowedEvidence> evidence,
        const ContextBuilderOptions&   opts = {}) const;
};

} // namespace wikore::rag
