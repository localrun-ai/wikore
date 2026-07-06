#include "wikore/rag/context_builder.hpp"
#include <format>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <spdlog/spdlog.h>

namespace wikore::rag {

namespace {

constexpr std::size_t kChatTemplateAllowance = 32;

// C++23 deducing-this alternative to the std::visit overloaded pattern.
// Used to type-dispatch across the AllowedEvidence variant without a
// generic lambda that would compile equally for AllowedChunk and
// AllowedRelationship — we want per-alternative code.
template<class... Ts> struct overloaded : Ts... { using Ts::operator()...; };
template<class... Ts> overloaded(Ts...) -> overloaded<Ts...>;

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

// Prompt-safety escape. Applied to every string we splice from live PG
// data (chunk text, section heading, edge_type, review_state, origin,
// direction, role) so a source that contains angle brackets, ampersands,
// or double quotes cannot appear to close the surrounding <source> tag,
// inject markup, or break out of a quoted attribute (section headings
// are rendered inside double quotes).
std::string escape(std::string_view text)
{
    std::string escaped;
    escaped.reserve(text.size());
    for (const char c : text) {
        switch (c) {
        case '&':  escaped += "&amp;";  break;
        case '<':  escaped += "&lt;";   break;
        case '>':  escaped += "&gt;";   break;
        case '"':  escaped += "&quot;"; break;
        default:   escaped += c;        break;
        }
    }
    return escaped;
}

// Formats a chunk as an evidence block. `n` is the 1-based [SRC N] index.
std::string format_chunk_block(int n, std::string_view text)
{
    return std::format(
        "[SRC {}]\n<source>\n{}\n</source>\n\n",
        n, escape(text));
}

// Formats an endpoint sub-block for the relationship. The endpoint chunk
// itself is emitted as a separate [SRC N] via format_chunk_block (or
// reused if already emitted); this only produces the "Endpoint A [SRC N]"
// reference line inside the [REL] block, plus a section heading hint
// when available so the LLM can identify context without ambiguity.
std::string format_endpoint_line(
    std::string_view label, int src_n,
    const std::optional<std::string>& section_heading)
{
    if (section_heading && !section_heading->empty()) {
        return std::format("Endpoint {} [SRC {}], section \"{}\"\n",
                           label, src_n, escape(*section_heading));
    }
    return std::format("Endpoint {} [SRC {}]\n", label, src_n);
}

// Emits the [REL] block for a relationship. The endpoints are referred
// to by their 1-based [SRC N] index; the caller is responsible for
// ensuring both indices point at emitted chunk blocks.
//
// Format matches docs §"Context construction and answer semantics"
// verbatim in shape — [REL Rn: type, origin=..., review_state=...,
// confidence=...] plus per-endpoint lines and a direction hint.
//
// Direction orientation for 'directed' edges is driven by ROLE, not by
// wire-ordinal. V034 stores the role of each endpoint (source / target
// / subject / object / a / b) independently of the ordinal — an edge
// legitimately created as ordinal0=target, ordinal1=source must render
// as "source implements target", not "target implements source". Rules:
//   * If exactly one endpoint has role in {source, subject}, that one
//     is on the semantic LEFT of the arrow.
//   * Otherwise (both source/subject, both target/object, both a/b,
//     or an unknown role) fall back to ordinal order (ordinal 0 left).
// This also future-proofs rendering for the V2 typed-endpoint work.
std::string format_relationship_block(
    int                        rel_n,
    const AllowedRelationship& r,
    int                        src_a_wire,   // [SRC N] of ordinal 0
    int                        src_b_wire)   // [SRC N] of ordinal 1
{
    // Decide semantic-left endpoint by role. `left_is_ordinal0` means
    // ordinal 0's role is the acting/source side; false means we flip
    // to render ordinal 1 on the left of the arrow.
    const auto is_left_role = [](std::string_view role) {
        return role == "source" || role == "subject";
    };
    const bool r0_left = is_left_role(r.endpoint_0().role);
    const bool r1_left = is_left_role(r.endpoint_1().role);
    // Only flip when exactly one endpoint is a left-role; ambiguity
    // (both or neither) falls back to wire ordinal.
    const bool left_is_ordinal0 = !(r1_left && !r0_left);

    const int src_left  = left_is_ordinal0 ? src_a_wire : src_b_wire;
    const int src_right = left_is_ordinal0 ? src_b_wire : src_a_wire;
    const auto& ep_left  = left_is_ordinal0 ? r.endpoint_0() : r.endpoint_1();
    const auto& ep_right = left_is_ordinal0 ? r.endpoint_1() : r.endpoint_0();

    std::string block;
    block += std::format(
        "[REL {}: {}, origin={}, review_state={}, confidence={:.2f}]\n",
        rel_n,
        escape(r.edge_type()),
        escape(r.origin()),
        escape(r.review_state()),
        r.confidence());
    // Endpoint labels A/B follow the semantic-left/right order after
    // role-based reorientation. Downstream citation targets [SRC N],
    // which is the same physical chunk either way.
    block += format_endpoint_line("A", src_left,  ep_left.section_heading);
    block += format_endpoint_line("B", src_right, ep_right.section_heading);

    // Direction line. The V034 CHECK constrains direction to
    // {'directed','symmetric'}. For directed we spell out
    // "[SRC left] {edge_type} [SRC right]" so the model does not have
    // to infer role from ordinal. For symmetric we say so.
    if (r.direction() == "symmetric") {
        block += "Direction: symmetric\n";
    } else {
        // Defensive default: treat anything non-'symmetric' as directed
        // rather than leaking a mystery value into the prompt.
        block += std::format("Direction: [SRC {}] {} [SRC {}]\n",
                             src_left, escape(r.edge_type()), src_right);
    }
    block += "\n";
    return block;
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
    // have been selected for the prompt. Also enforce the whole-
    // relationship invariant defensively: a hand-constructed
    // AllowedRelationship with mismatched endpoint tenants would be a
    // security-relevant bug even though the gate cannot produce one.
    for (const auto& ev : evidence) {
        const auto& company_id = std::visit(
            overloaded{
                [](const AllowedChunk& c) -> const std::string& {
                    return c.company_id();
                },
                [](const AllowedRelationship& r) -> const std::string& {
                    return r.company_id();
                },
            }, ev);
        if (company_id != ctx.tenant.company_id) {
            spdlog::error("[context-builder] cross-tenant evidence: "
                          "evidence company={} request company={}",
                          company_id, ctx.tenant.company_id);
            return std::unexpected(Error::invalid_state(
                "context_builder: evidence company_id does not match request tenant"));
        }
        // Both endpoints must reference the same tenant. The gate
        // enforces this by construction (single company_id in the SQL),
        // but treat it as an invariant here too.
        if (std::holds_alternative<AllowedRelationship>(ev)) {
            const auto& rel = std::get<AllowedRelationship>(ev);
            if (rel.endpoint_0().chunk_id.empty()
                || rel.endpoint_1().chunk_id.empty()) {
                spdlog::error("[context-builder] relationship {} missing endpoint chunk_id",
                              rel.edge_id());
                return std::unexpected(Error::invalid_state(
                    "context_builder: AllowedRelationship endpoint missing chunk_id"));
            }
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

    // Rendering state. The evidence stream is emitted in candidate
    // order, deduplicating on chunk_id so the same chunk cited by two
    // different relationships gets exactly one [SRC N] entry. Both
    // chunks-as-standalone-evidence and chunks-as-endpoints share this
    // map, per docs §"Context construction": "Repeating the same chunk
    // for multiple edges should be deduplicated."
    std::string evidence_blocks;
    std::unordered_map<std::string, int> chunk_src_index;   // chunk_id -> 1-based N
    int next_src = 1;
    int next_rel = 1;
    int included = 0;
    int excluded = 0;
    std::optional<Error> counter_error;

    // Check whether appending `additions` to the current evidence
    // blocks would still fit both byte and token budgets. Returns true
    // if it fits. Sets `counter_error` on token-counter failure so the
    // caller can propagate.
    auto fits = [&](std::string_view additions) -> bool {
        const std::string candidate_user =
            evidence_blocks + std::string(additions) + query_block;
        if (prompt_bytes(opts.system_prompt, candidate_user) > opts.max_prompt_bytes)
            return false;
        auto tokens = token_counter_->count(opts.system_prompt, candidate_user);
        if (!tokens) { counter_error = tokens.error(); return false; }
        return *tokens <= input_token_budget;
    };

    for (const auto& ev : evidence) {
        if (included >= opts.max_evidence_items) {
            ++excluded;
            continue;
        }

        bool accepted = false;

        std::visit(overloaded{
            [&](const AllowedChunk& chunk) {
                // If this chunk was already emitted (as an endpoint of
                // a previously-cited relationship), reuse its [SRC N]
                // instead of duplicating the block. `included` still
                // counts as +1 so the item-cap applies uniformly, and
                // we record the chunk_id in source_chunk_ids exactly
                // once (guarded by the map).
                auto it = chunk_src_index.find(chunk.chunk_id());
                if (it != chunk_src_index.end()) {
                    accepted = true;
                    return;
                }
                const std::string block = format_chunk_block(next_src, chunk.text());
                if (!fits(block)) return;
                evidence_blocks += block;
                chunk_src_index.emplace(chunk.chunk_id(), next_src++);
                out.source_chunk_ids.push_back(chunk.chunk_id());
                accepted = true;
            },
            [&](const AllowedRelationship& rel) {
                // All-or-nothing budget check: compute any missing
                // endpoint [SRC N] blocks plus the [REL N] block as a
                // single batch. If the batch does not fit, roll back —
                // no orphan endpoint left in the prompt without its
                // relationship (which would emit an unlabeled chunk
                // the LLM sees as evidence for the question).
                //
                // SRC numbers are assigned *speculatively* (tracking
                // pending additions) so the two endpoints get
                // contiguous labels even before we know the batch
                // fits; if it does not, we simply drop the pending
                // entries without touching next_src or the dedup map.
                struct Pending {
                    int         src_n;    // 1-based
                    std::string chunk_id;
                    std::string text;
                };
                std::vector<Pending> pending;
                pending.reserve(2);

                auto assign = [&](const std::string& chunk_id,
                                  std::string_view text) -> int {
                    auto it = chunk_src_index.find(chunk_id);
                    if (it != chunk_src_index.end()) return it->second;
                    // Look up in pending (same chunk on both endpoints
                    // would be legal only for a symmetric self-edge —
                    // the repo currently rejects those, but treat the
                    // case defensively so the label stays consistent).
                    for (const auto& p : pending)
                        if (p.chunk_id == chunk_id) return p.src_n;
                    const int n = next_src + static_cast<int>(pending.size());
                    pending.push_back({n, chunk_id, std::string(text)});
                    return n;
                };

                const int src_a = assign(rel.endpoint_0().chunk_id,
                                          rel.endpoint_0().text);
                const int src_b = assign(rel.endpoint_1().chunk_id,
                                          rel.endpoint_1().text);

                std::string all_blocks;
                for (const auto& p : pending)
                    all_blocks += format_chunk_block(p.src_n, p.text);
                all_blocks += format_relationship_block(
                    next_rel, rel, src_a, src_b);

                if (!fits(all_blocks)) return;
                evidence_blocks += all_blocks;
                for (auto& p : pending) {
                    chunk_src_index.emplace(p.chunk_id, p.src_n);
                    out.source_chunk_ids.push_back(std::move(p.chunk_id));
                }
                next_src += static_cast<int>(pending.size());
                out.source_edge_ids.push_back(rel.edge_id());
                ++next_rel;
                accepted = true;
            },
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
    out.evidence_included = included;
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
