#include "wikore/rag/knowledge_edge_repo.hpp"
#include "wikore/adapters/postgres/error_mapper.hpp"
#include <drogon/orm/Exception.h>
#include <spdlog/spdlog.h>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace wikore::rag {

namespace {

// Column list shared by all edge SELECTs. Endpoints are joined separately.
constexpr auto kEdgeCols =
    "id::text                        AS id,"
    "company_id::text                AS company_id,"
    "edge_type,"
    "direction,"
    "confidence,"
    "origin,"
    "review_state,"
    "provenance::text                AS provenance,"
    "formula_version,"
    "edge_version,"
    "created_by::text                AS created_by,"
    "reviewed_by::text               AS reviewed_by,"
    "created_at::text                AS created_at,"
    "reviewed_at::text               AS reviewed_at,"
    "expires_at::text                AS expires_at,"
    "superseded_at::text             AS superseded_at";

std::optional<std::string> nullable_text(const drogon::orm::Row& r, const char* col)
{
    return r[col].isNull() ? std::nullopt
                           : std::optional<std::string>(r[col].as<std::string>());
}

// Hydrate one edge row (no endpoints yet). Returns an Error on schema
// drift (an enum value not in the C++ table) rather than substituting
// a default — silently rendering, e.g., 'cites' as 'references' would
// misrepresent stored data instead of surfacing the drift.
Result<domain::KnowledgeEdge> hydrate_edge(const drogon::orm::Row& r)
{
    auto parse_or_fail = [&r]<typename P>(P parse_fn, const char* col, const char* what)
        -> Result<std::decay_t<decltype(*parse_fn(std::string_view{}))>>
    {
        auto s = r[col].as<std::string>();
        auto v = parse_fn(s);
        if (!v) {
            spdlog::error("[knowledge_edge_repo] schema drift: unexpected {}='{}'",
                          col, s);
            return std::unexpected(Error::database_error(std::format(
                "knowledge_edge row has unrecognised {} '{}' — schema drift", what, s)));
        }
        return *v;
    };

    auto edge_type    = parse_or_fail(domain::parse_edge_type,    "edge_type",    "edge_type");
    if (!edge_type)    return std::unexpected(edge_type.error());
    auto direction    = parse_or_fail(domain::parse_direction,    "direction",    "direction");
    if (!direction)    return std::unexpected(direction.error());
    auto origin       = parse_or_fail(domain::parse_origin,       "origin",       "origin");
    if (!origin)       return std::unexpected(origin.error());
    auto review_state = parse_or_fail(domain::parse_review_state, "review_state", "review_state");
    if (!review_state) return std::unexpected(review_state.error());

    return domain::KnowledgeEdge{
        .id           = r["id"].as<std::string>(),
        .company_id   = r["company_id"].as<std::string>(),
        .edge_type    = *edge_type,
        .direction    = *direction,
        .confidence   = r["confidence"].as<double>(),
        .origin       = *origin,
        .review_state = *review_state,
        .provenance_json = r["provenance"].isNull() ? "{}"
                                                    : r["provenance"].as<std::string>(),
        .formula_version = r["formula_version"].as<int>(),
        .edge_version    = r["edge_version"].as<long long>(),
        .created_by      = nullable_text(r, "created_by"),
        .reviewed_by     = nullable_text(r, "reviewed_by"),
        .created_at      = r["created_at"].as<std::string>(),
        .reviewed_at     = nullable_text(r, "reviewed_at"),
        .expires_at      = nullable_text(r, "expires_at"),
        .superseded_at   = nullable_text(r, "superseded_at"),
        .endpoints       = {},
    };
}

// Load endpoints for a set of edge ids, keyed by edge_id.
drogon::Task<Result<std::unordered_map<std::string, std::vector<domain::KnowledgeEdgeEndpoint>>>>
load_endpoints(drogon::orm::DbClientPtr db,
               std::string_view         company_id,
               const std::vector<std::string>& edge_ids)
{
    std::unordered_map<std::string, std::vector<domain::KnowledgeEdgeEndpoint>> out;
    if (edge_ids.empty()) co_return out;

    // ANY($2::uuid[]) accepts a text[] literal for parameter binding.
    std::string arr = "{";
    for (size_t i = 0; i < edge_ids.size(); ++i) {
        if (i) arr += ',';
        arr += edge_ids[i];
    }
    arr += '}';

    try {
        std::string company_id_arg{company_id};
        auto rows = co_await db->execSqlCoro(
            "SELECT edge_id::text AS edge_id, ordinal, chunk_id::text AS chunk_id, role "
            "FROM knowledge_edge_endpoints "
            "WHERE company_id = $1::uuid AND edge_id = ANY($2::uuid[]) "
            "ORDER BY edge_id, ordinal",
            company_id_arg, arr);
        for (const auto& r : rows) {
            auto role = domain::parse_role(r["role"].as<std::string>());
            if (!role) {
                spdlog::error("[knowledge_edge_repo] unexpected role='{}' — schema drift?",
                              r["role"].as<std::string>());
                continue;
            }
            out[r["edge_id"].as<std::string>()].push_back({
                .ordinal  = r["ordinal"].as<int>(),
                .chunk_id = r["chunk_id"].as<std::string>(),
                .role     = *role,
            });
        }
    } catch (const drogon::orm::DrogonDbException& ex) {
        co_return std::unexpected(postgres::map_db_exception(ex));
    }
    co_return out;
}

} // namespace

drogon::Task<Result<domain::KnowledgeEdge>>
KnowledgeEdgeRepo::create(std::string_view                     company_id,
                          const Uuid&                          actor_user_id,
                          const domain::CreateKnowledgeEdgeCmd& cmd)
{
    // Structural validation the DB CHECKs cannot express as cleanly.
    if (cmd.confidence < 0.0 || cmd.confidence > 1.0)
        co_return std::unexpected(Error::invalid_input(
            "knowledge_edge.confidence must be in [0.0, 1.0]"));
    if (cmd.endpoint_0.chunk_id == cmd.endpoint_1.chunk_id)
        co_return std::unexpected(Error::invalid_input(
            "knowledge_edge endpoints must reference different chunks"));
    if (cmd.endpoint_0.ordinal != 0 || cmd.endpoint_1.ordinal != 1)
        co_return std::unexpected(Error::invalid_input(
            "knowledge_edge endpoint ordinals must be exactly 0 and 1"));
    if (!domain::looks_like_uuid(cmd.endpoint_0.chunk_id) ||
        !domain::looks_like_uuid(cmd.endpoint_1.chunk_id))
        co_return std::unexpected(Error::invalid_input(
            "knowledge_edge endpoint chunk_id is not a valid UUID"));

    const auto review = cmd.review_state.value_or(domain::EdgeReviewState::accepted);
    const bool review_stamps =
        review == domain::EdgeReviewState::accepted  ||
        review == domain::EdgeReviewState::rejected  ||
        review == domain::EdgeReviewState::superseded;
    // superseded lifecycle timestamp is separate from reviewed_at: V034's
    // history captures edge_superseded_at for as-of reconstruction, so a
    // superseded row must carry a non-NULL timestamp or the history is a
    // lie. Stamp it whenever review_state moves to superseded.
    const bool superseded_now = review == domain::EdgeReviewState::superseded;

    // Data-modifying CTE: insert edge, then insert both endpoints in one
    // statement. Runs as a single implicit transaction so the deferred
    // two-endpoint CONSTRAINT TRIGGER fires at statement commit and any
    // failure surfaces synchronously. RETURNING is on the LAST INSERT so
    // we can capture something; the actual edge fields are re-read below
    // to avoid duplicating column marshalling.
    //
    // expires_at is passed as text with '' meaning NULL (cast to
    // timestamptz in a CASE) so we can keep a fixed varargs signature.
    // Booleans are passed as int (0/1) and cast to boolean in SQL —
    // drogon's SqlBinder does not have a native bool overload.
    //
    // Every string arg is a plain `std::string` lvalue, never a
    // `const std::string` prvalue (e.g. from a ternary), because drogon
    // only specialises `operator<<(std::string&&)` for non-const rvalues
    // and the generic T&& template fails to compile for std::string
    // ("invalid cast from std::string to uint16_t").
    std::string company_id_arg  {company_id};
    std::string edge_type_arg   {domain::to_wire(cmd.edge_type)};
    std::string direction_arg   {domain::to_wire(cmd.direction)};
    std::string origin_arg      {domain::to_wire(cmd.origin)};
    std::string review_arg      {domain::to_wire(review)};
    std::string provenance_arg  = cmd.provenance_json.empty()
                                    ? std::string("{}")
                                    : cmd.provenance_json;
    std::string exp_arg         = cmd.expires_at.value_or(std::string());
    std::string chunk0_arg      = cmd.endpoint_0.chunk_id;
    std::string chunk1_arg      = cmd.endpoint_1.chunk_id;
    std::string role0_arg       {domain::to_wire(cmd.endpoint_0.role)};
    std::string role1_arg       {domain::to_wire(cmd.endpoint_1.role)};
    std::string actor_arg       = actor_user_id;

    std::string edge_id;
    try {
        auto rows = co_await db_->execSqlCoro(
            "WITH new_edge AS ("
            "  INSERT INTO knowledge_edges "
            "    (company_id, edge_type, direction, confidence, origin, "
            "     review_state, provenance, created_by, reviewed_by, reviewed_at, "
            "     expires_at, superseded_at) "
            "  VALUES "
            "    ($1::uuid, $2, $3, $4::numeric, $5, $6, "
            "     COALESCE($7::jsonb, '{}'::jsonb), $8::uuid, "
            "     CASE WHEN $9::int  = 1 THEN $8::uuid ELSE NULL END, "
            "     CASE WHEN $9::int  = 1 THEN now()   ELSE NULL END, "
            "     CASE WHEN $10::text = '' THEN NULL ELSE $10::timestamptz END, "
            "     CASE WHEN $15::int  = 1 THEN now() ELSE NULL END) "
            "  RETURNING id, company_id"
            ") "
            "INSERT INTO knowledge_edge_endpoints (company_id, edge_id, ordinal, chunk_id, role) "
            "SELECT company_id, id, 0, $11::uuid, $12 FROM new_edge "
            "UNION ALL "
            "SELECT company_id, id, 1, $13::uuid, $14 FROM new_edge "
            "RETURNING edge_id::text AS edge_id",
            company_id_arg,
            edge_type_arg,
            direction_arg,
            cmd.confidence,
            origin_arg,
            review_arg,
            provenance_arg,
            actor_arg,
            static_cast<int>(review_stamps),
            exp_arg,
            chunk0_arg, role0_arg,
            chunk1_arg, role1_arg,
            static_cast<int>(superseded_now));
        if (rows.empty())
            co_return std::unexpected(Error::database_error("create returned no rows"));
        edge_id = rows[0]["edge_id"].as<std::string>();
    } catch (const drogon::orm::DrogonDbException& ex) {
        co_return std::unexpected(postgres::map_db_exception(ex));
    }
    co_return co_await get(company_id, edge_id);
}

drogon::Task<Result<domain::KnowledgeEdge>>
KnowledgeEdgeRepo::get(std::string_view company_id, std::string_view edge_id)
{
    if (!domain::looks_like_uuid(edge_id))
        co_return std::unexpected(Error::not_found("knowledge_edge not found"));

    // Fetch the edge row and its endpoints in two SELECTs. Structured so
    // that `co_await` never crosses `try`/`catch` boundaries — GCC 14
    // hits an ICE (build_special_member_call at cp/call.cc:11096) on
    // certain arrangements of coroutine suspension inside try blocks,
    // and the reset here keeps the frame flat.
    domain::KnowledgeEdge edge;
    {
        std::string edge_id_arg   {edge_id};
        std::string company_id_arg{company_id};
        drogon::orm::Result rows{nullptr};
        try {
            rows = co_await db_->execSqlCoro(
                std::format("SELECT {} FROM knowledge_edges "
                            "WHERE id = $1::uuid AND company_id = $2::uuid",
                            kEdgeCols),
                edge_id_arg, company_id_arg);
        } catch (const drogon::orm::DrogonDbException& ex) {
            co_return std::unexpected(postgres::map_db_exception(ex));
        }
        if (rows.empty())
            co_return std::unexpected(Error::not_found("knowledge_edge not found"));
        auto hydrated = hydrate_edge(rows[0]);
        if (!hydrated) co_return std::unexpected(hydrated.error());
        edge = std::move(*hydrated);
    }

    std::vector<std::string> ids;
    ids.push_back(edge.id);
    auto ends = co_await load_endpoints(db_, company_id, ids);
    if (!ends) co_return std::unexpected(ends.error());
    if (auto it = ends->find(edge.id); it != ends->end())
        edge.endpoints = std::move(it->second);
    co_return edge;
}

drogon::Task<Result<std::vector<domain::KnowledgeEdge>>>
KnowledgeEdgeRepo::list(std::string_view                        company_id,
                        const domain::ListKnowledgeEdgesFilter& filter)
{
    // Static-shape SQL: every filter is a "NULL means match all" clause so
    // we can use a fixed varargs signature to execSqlCoro. Predicates are
    // planner-friendly (equality against an indexed column when set,
    // dropped as trivially true when NULL).
    const std::string edge_type =
        filter.edge_type ? std::string(domain::to_wire(*filter.edge_type)) : std::string();
    const std::string review_state =
        filter.review_state ? std::string(domain::to_wire(*filter.review_state)) : std::string();
    const std::string chunk_id = filter.endpoint_chunk_id.value_or(std::string());

    // Same coroutine-frame shape as get(): keep co_await outside try blocks
    // where possible to avoid the GCC 14 ICE (build_special_member_call at
    // cp/call.cc:11096) that some try/co_return/co_await combinations
    // trigger in coroutine-frame emission.
    std::vector<domain::KnowledgeEdge> out;
    std::vector<std::string>          ids;
    {
        std::string company_id_arg{company_id};
        drogon::orm::Result rows{nullptr};
        try {
            rows = co_await db_->execSqlCoro(
                std::format(
                    "SELECT {} FROM knowledge_edges "
                    "WHERE company_id = $1::uuid "
                    "  AND ($2::text = '' OR edge_type    = $2::text) "
                    "  AND ($3::text = '' OR review_state = $3::text) "
                    "  AND ($4::text = '' OR id IN ("
                    "        SELECT edge_id FROM knowledge_edge_endpoints "
                    "        WHERE company_id = $1::uuid "
                    "          AND chunk_id   = $4::uuid)) "
                    "ORDER BY created_at DESC, id ASC "
                    "LIMIT $5::int OFFSET $6::int",
                    kEdgeCols),
                company_id_arg,
                edge_type,
                review_state,
                chunk_id,
                filter.limit,
                filter.offset);
        } catch (const drogon::orm::DrogonDbException& ex) {
            co_return std::unexpected(postgres::map_db_exception(ex));
        }
        out.reserve(rows.size());
        ids.reserve(rows.size());
        for (const auto& r : rows) {
            auto e = hydrate_edge(r);
            if (!e) co_return std::unexpected(e.error());
            ids.push_back(e->id);
            out.push_back(std::move(*e));
        }
    }

    auto ends = co_await load_endpoints(db_, company_id, ids);
    if (!ends) co_return std::unexpected(ends.error());
    for (auto& e : out) {
        if (auto it = ends->find(e.id); it != ends->end())
            e.endpoints = std::move(it->second);
    }
    co_return out;
}

drogon::Task<Result<domain::KnowledgeEdge>>
KnowledgeEdgeRepo::update(std::string_view                      company_id,
                          const Uuid&                           actor_user_id,
                          std::string_view                      edge_id,
                          const domain::UpdateKnowledgeEdgeCmd& cmd)
{
    if (!domain::looks_like_uuid(edge_id))
        co_return std::unexpected(Error::not_found("knowledge_edge not found"));

    // No-op update: skip the AFTER UPDATE history-trigger firing.
    if (!cmd.confidence && !cmd.review_state && !cmd.provenance_json && !cmd.expires_at) {
        co_return co_await get(company_id, edge_id);
    }
    if (cmd.confidence && (*cmd.confidence < 0.0 || *cmd.confidence > 1.0))
        co_return std::unexpected(Error::invalid_input(
            "knowledge_edge.confidence must be in [0.0, 1.0]"));

    // Static-shape UPDATE using CASE/COALESCE so we can bind a fixed
    // parameter list without stitching SQL together. Sentinels:
    //   * numeric confidence:   -1  = untouched (valid range is [0,1])
    //   * text review_state:    ''  = untouched
    //   * jsonb provenance:     NULL bind = untouched (jsonb has no ''
    //                                shortcut without ambiguity)
    //   * text expires_at:      NULL bind = untouched, '' = clear,
    //                           otherwise timestamptz-cast
    //   * boolean review_stamps: true = also stamp reviewed_by, reviewed_at
    const double conf_arg =
        cmd.confidence ? *cmd.confidence : -1.0;
    std::string rs_arg =
        cmd.review_state ? std::string(domain::to_wire(*cmd.review_state)) : std::string();
    const bool review_stamps = cmd.review_state && (
        *cmd.review_state == domain::EdgeReviewState::accepted  ||
        *cmd.review_state == domain::EdgeReviewState::rejected  ||
        *cmd.review_state == domain::EdgeReviewState::superseded);
    // Same rationale as create(): a superseded row must carry a non-NULL
    // superseded_at so history-based as-of reconstruction can tell it was
    // superseded. Stamp it iff we are transitioning to superseded.
    // Note: moving back to a non-superseded state leaves the historical
    // superseded_at intact — that is deliberate; history is append-only.
    // (Same intentional stickiness applies to reviewed_by/reviewed_at.)
    const bool superseded_now = cmd.review_state && *cmd.review_state == domain::EdgeReviewState::superseded;

    try {
        // Provenance and expires_at use a two-bind pair (a NULL sentinel
        // and the value) because drogon's parameter binding does not let
        // us express "NULL means untouched" cleanly for jsonb/timestamp in
        // a single bind. `prov_present`/`exp_present` toggles are the
        // explicit "did the caller send this field?" bit. Booleans are
        // passed as int (0/1) because drogon's SqlBinder has no bool
        // overload. Every string arg is a plain non-const std::string
        // lvalue — see create() for why const-string prvalues fail to
        // compile against drogon's operator<<.
        const int prov_present = cmd.provenance_json.has_value() ? 1 : 0;
        const int exp_present  = cmd.expires_at.has_value() ? 1 : 0;
        std::string prov_arg    = cmd.provenance_json.value_or("{}");
        std::string exp_arg     = cmd.expires_at.value_or("");
        std::string actor_arg   = actor_user_id;
        std::string edge_id_arg {edge_id};
        std::string company_id_arg{company_id};

        auto rows = co_await db_->execSqlCoro(
            "UPDATE knowledge_edges SET "
            "  edge_version = edge_version + 1, "
            "  confidence   = CASE WHEN $1::numeric >= 0 THEN $1::numeric ELSE confidence END, "
            "  review_state = CASE WHEN $2::text = ''    THEN review_state ELSE $2::text END, "
            "  provenance   = CASE WHEN $3::int = 1      THEN $4::jsonb    ELSE provenance END, "
            "  expires_at   = CASE "
            "                   WHEN $5::int = 0     THEN expires_at "
            "                   WHEN $6::text  = ''  THEN NULL "
            "                   ELSE $6::timestamptz "
            "                 END, "
            "  reviewed_by  = CASE WHEN $7::int = 1  THEN $8::uuid ELSE reviewed_by END, "
            "  reviewed_at  = CASE WHEN $7::int = 1  THEN now()    ELSE reviewed_at END, "
            "  superseded_at = CASE WHEN $11::int = 1 THEN now()   ELSE superseded_at END "
            "WHERE id = $9::uuid AND company_id = $10::uuid "
            "RETURNING id::text AS id",
            conf_arg,
            rs_arg,
            prov_present, prov_arg,
            exp_present,  exp_arg,
            static_cast<int>(review_stamps), actor_arg,
            edge_id_arg, company_id_arg,
            static_cast<int>(superseded_now));
        if (rows.empty())
            co_return std::unexpected(Error::not_found("knowledge_edge not found"));
    } catch (const drogon::orm::DrogonDbException& ex) {
        co_return std::unexpected(postgres::map_db_exception(ex));
    }
    co_return co_await get(company_id, edge_id);
}

drogon::Task<Result<void>>
KnowledgeEdgeRepo::remove(std::string_view company_id, std::string_view edge_id)
{
    if (!domain::looks_like_uuid(edge_id))
        co_return std::unexpected(Error::not_found("knowledge_edge not found"));

    try {
        std::string edge_id_arg   {edge_id};
        std::string company_id_arg{company_id};
        auto rows = co_await db_->execSqlCoro(
            "DELETE FROM knowledge_edges "
            "WHERE id = $1::uuid AND company_id = $2::uuid "
            "RETURNING id::text AS id",
            edge_id_arg, company_id_arg);
        if (rows.empty())
            co_return std::unexpected(Error::not_found("knowledge_edge not found"));
    } catch (const drogon::orm::DrogonDbException& ex) {
        co_return std::unexpected(postgres::map_db_exception(ex));
    }
    co_return Result<void>{};
}

} // namespace wikore::rag
