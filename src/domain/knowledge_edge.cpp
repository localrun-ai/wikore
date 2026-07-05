#include "wikore/domain/knowledge_edge.hpp"
#include <array>
#include <cctype>
#include <string_view>
#include <utility>

namespace wikore::domain {

namespace {

template <typename E, size_t N>
std::optional<E> parse_enum(std::string_view s,
                            const std::array<std::pair<std::string_view, E>, N>& table)
{
    for (const auto& [wire, value] : table)
        if (s == wire) return value;
    return std::nullopt;
}

constexpr std::array<std::pair<std::string_view, EdgeType>, 9> kEdgeTypeTable{{
    {"implements",             EdgeType::implements},
    {"depends_on",             EdgeType::depends_on},
    {"exception_to",           EdgeType::exception_to},
    {"contradicts",            EdgeType::contradicts},
    {"same_requirement_as",    EdgeType::same_requirement_as},
    {"derived_from",           EdgeType::derived_from},
    {"cites",                  EdgeType::cites},
    {"affects",                EdgeType::affects},
    {"requires_approval_from", EdgeType::requires_approval_from},
}};

constexpr std::array<std::pair<std::string_view, EdgeDirection>, 2> kDirectionTable{{
    {"directed",  EdgeDirection::directed},
    {"symmetric", EdgeDirection::symmetric},
}};

constexpr std::array<std::pair<std::string_view, EdgeOrigin>, 4> kOriginTable{{
    {"parser",             EdgeOrigin::parser},
    {"deterministic_rule", EdgeOrigin::deterministic_rule},
    {"administrator",      EdgeOrigin::administrator},
    {"llm_proposal",       EdgeOrigin::llm_proposal},
}};

constexpr std::array<std::pair<std::string_view, EdgeReviewState>, 4> kReviewStateTable{{
    {"proposed",   EdgeReviewState::proposed},
    {"accepted",   EdgeReviewState::accepted},
    {"rejected",   EdgeReviewState::rejected},
    {"superseded", EdgeReviewState::superseded},
}};

constexpr std::array<std::pair<std::string_view, EndpointRole>, 6> kRoleTable{{
    {"source",  EndpointRole::source},
    {"target",  EndpointRole::target},
    {"subject", EndpointRole::subject},
    {"object",  EndpointRole::object},
    {"a",       EndpointRole::a},
    {"b",       EndpointRole::b},
}};

template <typename E, size_t N>
std::string_view to_wire_impl(E value,
                              const std::array<std::pair<std::string_view, E>, N>& table)
{
    for (const auto& [wire, v] : table)
        if (v == value) return wire;
    return "";  // unreachable if enum is exhaustive; keep signature total
}

} // namespace

std::string_view to_wire(EdgeType v)         { return to_wire_impl(v, kEdgeTypeTable); }
std::string_view to_wire(EdgeDirection v)    { return to_wire_impl(v, kDirectionTable); }
std::string_view to_wire(EdgeOrigin v)       { return to_wire_impl(v, kOriginTable); }
std::string_view to_wire(EdgeReviewState v)  { return to_wire_impl(v, kReviewStateTable); }
std::string_view to_wire(EndpointRole v)     { return to_wire_impl(v, kRoleTable); }

std::optional<EdgeType>        parse_edge_type(std::string_view s)    { return parse_enum(s, kEdgeTypeTable); }
std::optional<EdgeDirection>   parse_direction(std::string_view s)    { return parse_enum(s, kDirectionTable); }
std::optional<EdgeOrigin>      parse_origin(std::string_view s)       { return parse_enum(s, kOriginTable); }
std::optional<EdgeReviewState> parse_review_state(std::string_view s) { return parse_enum(s, kReviewStateTable); }
std::optional<EndpointRole>    parse_role(std::string_view s)         { return parse_enum(s, kRoleTable); }

// Canonical 8-4-4-4-12 UUID shape (32 hex + 4 hyphens = 36 chars).
bool looks_like_uuid(std::string_view s)
{
    if (s.size() != 36) return false;
    for (std::size_t i = 0; i < 36; ++i) {
        const char c = s[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') return false;
        } else if (!std::isxdigit(static_cast<unsigned char>(c))) {
            return false;
        }
    }
    return true;
}

} // namespace wikore::domain
