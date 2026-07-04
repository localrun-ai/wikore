#include "wikore/rag/context_builder.hpp"
#include <format>
#include <spdlog/spdlog.h>

namespace wikore::rag {

// Fraction of the budget reserved for the model's answer.
static constexpr std::size_t kAnswerHeadroomFraction = 4; // max_prompt_bytes / 4
static constexpr std::size_t kMinAnswerHeadroom      = 256;   // bytes
static constexpr std::size_t kMaxAnswerHeadroom      = 8192;  // bytes

Result<PromptContext> ContextBuilder::build(
    const wikore::RequestContext& ctx,
    std::string_view               query,
    std::span<const AllowedEvidence> evidence,
    const ContextBuilderOptions&   opts) const
{
    // -----------------------------------------------------------------------
    // 1. Validate options.
    // -----------------------------------------------------------------------
    if (opts.max_prompt_bytes == 0 || opts.max_prompt_bytes > kContextBuilderMaxBytes)
        return std::unexpected(Error::invalid_input(std::format(
            "context_builder: max_prompt_bytes must be in [1, {}]",
            kContextBuilderMaxBytes)));
    if (opts.max_evidence_items < 0)
        return std::unexpected(Error::invalid_input(
            "context_builder: max_evidence_items must be >= 0"));

    // -----------------------------------------------------------------------
    // 2. Reserve answer headroom, then check mandatory content fits.
    //    All arithmetic uses std::size_t to prevent signed overflow.
    // -----------------------------------------------------------------------
    const std::size_t headroom = std::clamp(
        opts.max_prompt_bytes / kAnswerHeadroomFraction,
        kMinAnswerHeadroom, kMaxAnswerHeadroom);
    std::size_t remaining = opts.max_prompt_bytes - headroom;

    const auto charge = [&](std::size_t n) -> bool {
        if (n > remaining) return false;
        remaining -= n;
        return true;
    };

    if (!opts.system_prompt.empty()) {
        const std::size_t sys_bytes = opts.system_prompt.size() + 2; // +2 for \n\n
        if (!charge(sys_bytes))
            return std::unexpected(Error::invalid_input(
                "context_builder: system_prompt exceeds byte budget"));
    }

    const std::string query_block = std::format("Question: {}\n\nAnswer:", query);
    if (!charge(query_block.size()))
        return std::unexpected(Error::invalid_input(
            "context_builder: query exceeds remaining byte budget "
            "(increase max_prompt_bytes or shorten the query)"));

    // -----------------------------------------------------------------------
    // 3. Build prompt — tenant-check each evidence item before inclusion.
    // -----------------------------------------------------------------------
    // reserve() capped: max_prompt_bytes validated <= 4 MiB above, so
    // max_prompt_bytes * 1 (no multiplier) is safe. We already have space
    // for the headroom fraction, so the actual prompt fits within the reserve.
    PromptContext out;
    out.prompt.reserve(opts.max_prompt_bytes);

    if (!opts.system_prompt.empty()) {
        out.prompt += opts.system_prompt;
        out.prompt += "\n\n";
    }

    int n = 0, truncated = 0;
    for (const auto& ev : evidence) {
        if (n >= opts.max_evidence_items) { ++truncated; continue; }

        // Tenant check — fail closed on mismatch; do not silently skip.
        const std::string* ev_company = std::visit(
            [](const AllowedChunk& c) -> const std::string* { return &c.company_id(); },
            ev);
        if (*ev_company != ctx.tenant.company_id) {
            spdlog::error("[context-builder] cross-tenant evidence: "
                          "evidence company={} request company={}",
                          *ev_company, ctx.tenant.company_id);
            return std::unexpected(Error::invalid_state(
                "context_builder: evidence company_id does not match request tenant"));
        }

        std::visit([&](const AllowedChunk& chunk) {
            // Byte cost: header "[SRC N]\n" + text + "\n\n"
            const std::size_t cost = 8 + chunk.text().size() + 2;
            if (!charge(cost)) { ++truncated; return; }
            out.prompt += std::format("[SRC {}]\n{}\n\n", ++n, chunk.text());
            out.source_chunk_ids.push_back(chunk.chunk_id());
        }, ev);
    }

    out.prompt += query_block;
    out.prompt_bytes = opts.max_prompt_bytes - headroom - remaining;

    if (truncated > 0)
        spdlog::debug("[context-builder] {}/{} items included, {} excluded by budget",
                      n, static_cast<int>(evidence.size()), truncated);
    else
        spdlog::debug("[context-builder] {} items, {} bytes", n, out.prompt_bytes);
    return out;
}

} // namespace wikore::rag
