#include "wikore/rag/context_builder.hpp"
#include <format>
#include <limits>
#include <stdexcept>
#include <spdlog/spdlog.h>

namespace wikore::rag {

namespace {

constexpr std::size_t kChatTemplateAllowance = 32;

bool add_overflows(std::size_t a, std::size_t b) noexcept
{
    return b > std::numeric_limits<std::size_t>::max() - a;
}

std::size_t prompt_bytes(std::string_view system_message,
                         std::string_view user_message)
{
    if (add_overflows(system_message.size(), user_message.size()))
        return std::numeric_limits<std::size_t>::max();
    return system_message.size() + user_message.size();
}

std::string escape_source_text(std::string_view text)
{
    std::string escaped;
    escaped.reserve(text.size());
    for (const char c : text) {
        switch (c) {
        case '&': escaped += "&amp;";  break;
        case '<': escaped += "&lt;";   break;
        case '>': escaped += "&gt;";   break;
        default:  escaped += c;        break;
        }
    }
    return escaped;
}

} // namespace

Result<std::size_t> ByteUpperBoundTokenCounter::count(
    std::string_view system_message,
    std::string_view user_message) const
{
    const auto bytes = prompt_bytes(system_message, user_message);
    if (bytes == std::numeric_limits<std::size_t>::max()
        || add_overflows(bytes, kChatTemplateAllowance))
        return std::unexpected(Error::invalid_input(
            "context_builder: prompt size overflow"));
    return bytes + kChatTemplateAllowance;
}

ContextBuilder::ContextBuilder(
    std::shared_ptr<const PromptTokenCounterPort> token_counter)
    : token_counter_(std::move(token_counter))
{
    if (!token_counter_)
        throw std::invalid_argument("ContextBuilder: token_counter is required");
}

Result<PromptContext> ContextBuilder::build(
    const wikore::RequestContext&    ctx,
    std::string_view                 query,
    std::span<const AllowedEvidence> evidence,
    const ContextBuilderOptions&     opts) const
{
    if (opts.max_context_tokens == 0
        || opts.max_context_tokens > kContextBuilderMaxTokens)
        return std::unexpected(Error::invalid_input(std::format(
            "context_builder: max_context_tokens must be in [1, {}]",
            kContextBuilderMaxTokens)));
    if (opts.reserved_output_tokens >= opts.max_context_tokens)
        return std::unexpected(Error::invalid_input(
            "context_builder: reserved_output_tokens must be smaller than max_context_tokens"));
    if (opts.max_prompt_bytes == 0
        || opts.max_prompt_bytes > kContextBuilderMaxBytes)
        return std::unexpected(Error::invalid_input(std::format(
            "context_builder: max_prompt_bytes must be in [1, {}]",
            kContextBuilderMaxBytes)));
    if (opts.max_evidence_items < 0)
        return std::unexpected(Error::invalid_input(
            "context_builder: max_evidence_items must be >= 0"));

    // Validate every supplied item before applying item or size caps. A
    // cross-tenant object is an evidence-routing bug even when it would not
    // have been selected for the prompt.
    for (const auto& ev : evidence) {
        const auto& company_id = std::visit(
            [](const AllowedChunk& c) -> const std::string& {
                return c.company_id();
            }, ev);
        if (company_id != ctx.tenant.company_id) {
            spdlog::error("[context-builder] cross-tenant evidence: "
                          "evidence company={} request company={}",
                          company_id, ctx.tenant.company_id);
            return std::unexpected(Error::invalid_state(
                "context_builder: evidence company_id does not match request tenant"));
        }
    }

    const std::size_t input_token_budget =
        opts.max_context_tokens - opts.reserved_output_tokens;
    const std::string query_block = std::format("Question: {}\n\nAnswer:", query);

    auto mandatory_tokens = token_counter_->count(opts.system_prompt, query_block);
    if (!mandatory_tokens)
        return std::unexpected(mandatory_tokens.error());
    if (*mandatory_tokens > input_token_budget)
        return std::unexpected(Error::invalid_input(
            "context_builder: system prompt and query exceed input token budget"));
    if (prompt_bytes(opts.system_prompt, query_block) > opts.max_prompt_bytes)
        return std::unexpected(Error::invalid_input(
            "context_builder: system prompt and query exceed byte budget"));

    PromptContext out;
    out.system_message = opts.system_prompt;
    out.user_message.reserve(opts.max_prompt_bytes);

    std::string evidence_blocks;
    int included = 0;
    int excluded = 0;
    std::optional<Error> counter_error;
    for (const auto& ev : evidence) {
        if (included >= opts.max_evidence_items) {
            ++excluded;
            continue;
        }

        bool accepted = false;
        std::visit([&](const AllowedChunk& chunk) {
            const std::string escaped_text = escape_source_text(chunk.text());
            const std::string block = std::format(
                "[SRC {}]\n<source>\n{}\n</source>\n\n",
                included + 1, escaped_text);
            const std::string candidate_user = evidence_blocks + block + query_block;
            if (prompt_bytes(opts.system_prompt, candidate_user)
                    > opts.max_prompt_bytes) {
                return;
            }
            auto tokens = token_counter_->count(opts.system_prompt, candidate_user);
            if (!tokens) {
                counter_error = tokens.error();
                return;
            }
            if (*tokens > input_token_budget)
                return;
            evidence_blocks += block;
            out.source_chunk_ids.push_back(chunk.chunk_id());
            accepted = true;
        }, ev);

        if (counter_error)
            return std::unexpected(std::move(*counter_error));

        if (accepted)
            ++included;
        else
            ++excluded;
    }

    out.user_message = std::move(evidence_blocks);
    out.user_message += query_block;
    out.prompt_bytes = prompt_bytes(out.system_message, out.user_message);
    auto final_tokens = token_counter_->count(out.system_message, out.user_message);
    if (!final_tokens)
        return std::unexpected(final_tokens.error());
    out.prompt_tokens = *final_tokens;

    if (excluded > 0)
        spdlog::debug("[context-builder] {}/{} items included, {} excluded",
                      included, static_cast<int>(evidence.size()), excluded);
    else
        spdlog::debug("[context-builder] {} items, {} tokens, {} bytes",
                      included, out.prompt_tokens, out.prompt_bytes);
    return out;
}

} // namespace wikore::rag
