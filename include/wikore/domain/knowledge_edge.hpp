#pragma once
#include "wikore/domain/types.hpp"
#include <optional>
#include <string>
#include <vector>

namespace wikore::domain {

// ---------------------------------------------------------------------------
// Knowledge edges (BaryGraph Lite V1 — chunk-to-chunk).
//
// See docs/barygraph_features.md §"Terminology" and §"knowledge_edges".
// V034 enforces the schema-level invariants (edge_type_v1_chk, role_check,
// exactly-two-endpoints, endpoint immutability, UUID no-reuse); this header
// declares the C++ shapes and thin validators the admin/edge CRUD path
// uses before hitting the DB.
// ---------------------------------------------------------------------------

// V1 edge type set — must stay in sync with V034's
// knowledge_edges_edge_type_v1_chk (chunk-to-chunk types only).
// Types requiring non-chunk endpoints (supersedes, section_*,
// responsible_team) are intentionally omitted; they will land with v2
// typed endpoints.
enum class EdgeType {
    implements,
    depends_on,
    exception_to,
    contradicts,
    same_requirement_as,
    derived_from,
    cites,
    affects,
    requires_approval_from,
};

enum class EdgeDirection {
    directed,
    symmetric,
};

enum class EdgeOrigin {
    parser,
    deterministic_rule,
    administrator,
    llm_proposal,
};

enum class EdgeReviewState {
    proposed,
    accepted,
    rejected,
    superseded,
};

// V034's knowledge_edge_endpoints_role_check enumerates these six.
enum class EndpointRole {
    source,
    target,
    subject,
    object,
    a,
    b,
};

std::string_view to_wire(EdgeType);
std::string_view to_wire(EdgeDirection);
std::string_view to_wire(EdgeOrigin);
std::string_view to_wire(EdgeReviewState);
std::string_view to_wire(EndpointRole);

std::optional<EdgeType>        parse_edge_type(std::string_view);
std::optional<EdgeDirection>   parse_direction(std::string_view);
std::optional<EdgeOrigin>      parse_origin(std::string_view);
std::optional<EdgeReviewState> parse_review_state(std::string_view);
std::optional<EndpointRole>    parse_role(std::string_view);

// Structural validation for a chunk_id (must be a canonical UUID). The
// caller (handler) has already applied HTTP-layer authz; the repo relies
// on Postgres FKs for tenant checking and CHECK constraints for enum
// validation, so this is only defense in depth against malformed input.
bool looks_like_uuid(std::string_view);

struct KnowledgeEdgeEndpoint {
    int          ordinal;      // 0 or 1
    Uuid         chunk_id;
    EndpointRole role;
};

struct KnowledgeEdge {
    Uuid                                       id;
    Uuid                                       company_id;
    EdgeType                                   edge_type;
    EdgeDirection                              direction;
    double                                     confidence;   // [0.0, 1.0]
    EdgeOrigin                                 origin;
    EdgeReviewState                            review_state;
    std::string                                provenance_json;   // raw JSONB
    int                                        formula_version;
    long long                                  edge_version;
    std::optional<Uuid>                        created_by;
    std::optional<Uuid>                        reviewed_by;
    std::string                                created_at;         // ISO8601
    std::optional<std::string>                 reviewed_at;
    std::optional<std::string>                 expires_at;
    std::optional<std::string>                 superseded_at;
    std::vector<KnowledgeEdgeEndpoint>         endpoints;          // size 2
};

// Command shapes for the admin CRUD path. Wire (JSON) shapes live in the
// handler; these are the validated, normalized values that reach the repo.

struct CreateKnowledgeEdgeCmd {
    EdgeType                                edge_type;
    EdgeDirection                           direction;
    double                                  confidence;
    EdgeOrigin                              origin;
    std::optional<EdgeReviewState>          review_state;         // defaults to accepted (admin path)
    std::string                             provenance_json;      // "{}" if none
    std::optional<std::string>              expires_at;           // ISO8601
    KnowledgeEdgeEndpoint                   endpoint_0;
    KnowledgeEdgeEndpoint                   endpoint_1;
};

struct UpdateKnowledgeEdgeCmd {
    // All fields optional — only present ones are updated. Endpoints and
    // structural fields (edge_type, direction, origin) are IMMUTABLE per
    // V034; attempts to change them return invalid_input.
    std::optional<double>                   confidence;
    std::optional<EdgeReviewState>          review_state;
    std::optional<std::string>              provenance_json;
    std::optional<std::string>              expires_at;           // pass "" to clear
    // review_state=accepted|rejected|superseded also stamps reviewed_by
    // (from the request principal) and reviewed_at (now()) in the repo.
};

struct ListKnowledgeEdgesFilter {
    std::optional<EdgeType>        edge_type;
    std::optional<EdgeReviewState> review_state;
    std::optional<Uuid>            endpoint_chunk_id;  // matches either ordinal
    int                            limit  = 50;
    int                            offset = 0;
};

} // namespace wikore::domain
