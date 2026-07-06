#pragma once
#include "wikore/rag/types.hpp"
#include "wikore/domain/types.hpp"
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace wikore::rag {

// Counts the complete structured prompt for the selected model. Provider-
// specific implementations may include chat-template overhead exactly.
class PromptTokenCounterPort {
public:
    virtual ~PromptTokenCounterPort() = default;

    [[nodiscard]] virtual Result<std::size_t>
    count(std::string_view system_message,
          std::string_view user_message) const = 0;
};

// Safe fallback until a model tokenizer is configured: a valid UTF-8 prompt
// cannot require more byte-fallback tokens than it has bytes, plus a small
// allowance for message-role/template markers.
class ByteUpperBoundTokenCounter final : public PromptTokenCounterPort {
public:
    [[nodiscard]] Result<std::size_t>
    count(std::string_view system_message,
          std::string_view user_message) const override;
};

struct PromptContext {
    // Keep roles separate so callers can construct real system/user
    // ChatMessages instead of flattening instructions into user content.
    std::string system_message;
    std::string user_message;

    // Chunk IDs cited in the prompt, in the order [SRC N] labels were
    // assigned. When a chunk appears both as standalone evidence AND as
    // a relationship endpoint, its ID appears exactly once (dedup keys
    // by chunk_id). The [SRC N] label of a chunk_id is `1 + index_of`
    // that chunk_id in this vector.
    std::vector<std::string> source_chunk_ids;

    // Edge IDs cited in the prompt, in the order [REL N] labels were
    // assigned. Empty for pure chunk (fact-intent) evidence.
    std::vector<std::string> source_edge_ids;

    std::size_t prompt_tokens = 0;
    std::size_t prompt_bytes  = 0;
};

inline constexpr std::size_t kContextBuilderMaxBytes =
    4UL * 1024UL * 1024UL;
inline constexpr std::size_t kContextBuilderMaxTokens = 131072;

struct ContextBuilderOptions {
    // Input and output are separate token budgets. The prompt must fit in
    // max_context_tokens - reserved_output_tokens.
    std::size_t max_context_tokens     = 8192;
    std::size_t reserved_output_tokens = 2048;
    // Independent allocation/transport bound, even when a tokenizer is used.
    std::size_t max_prompt_bytes       = 64UL * 1024UL;
    int         max_evidence_items     = 20;
    std::string system_prompt;
};

class ContextBuilder {
public:
    explicit ContextBuilder(
        std::shared_ptr<const PromptTokenCounterPort> token_counter =
            std::make_shared<ByteUpperBoundTokenCounter>());

    [[nodiscard]] Result<PromptContext> build(
        const wikore::RequestContext&    ctx,
        std::string_view                 query,
        std::span<const AllowedEvidence> evidence,
        const ContextBuilderOptions&     opts = {}) const;

private:
    std::shared_ptr<const PromptTokenCounterPort> token_counter_;
};

} // namespace wikore::rag
