#pragma once
#include "wikore/domain/types.hpp"
#include "wikore/rag/vector_store.hpp"
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace wikore::scheduler {

// ---------------------------------------------------------------------------
// EmbedEdgeWorker — drains qdrant_upsert_edge_vector events emitted by
// V037's knowledge_edges_enqueue_upsert_fn trigger. For each event:
//
//   1. Skip if the edge no longer exists (V037 5b-contract note: an
//      INSERT+DELETE-same-tx or a DELETE between enqueue and claim can
//      leave a stale event; treat "edge gone" as complete-and-no-op).
//
//   2. Skip as superseded if payload.edge_version <=
//      knowledge_edge_embeddings.indexed_edge_version for
//      (edge_id, embedding_model_id). Out-of-order redelivery guard;
//      mirrors ResyncWorker's CAS pattern from V032.
//
//   3. Load the endpoint chunks' qdrant_point_ids from
//      document_chunk_vectors (per the payload's embedding_model_id),
//      look up their vectors via VectorStorePort::search_by_ids on
//      that model's chunk collection, load the endpoint documents'
//      authority_level, load the type vector from edge_type_vectors,
//      and compute the formula-v1 edge vector.
//
//   4. Deterministic point id (V037 5b-contract, orphan-free formula):
//        uuid_v5(edge_id + ':' + embedding_model_id + ':' + formula_version)
//      Upsert to Qdrant edge collection with (company_id, edge_id,
//      edge_type, formula_version, edge_version, review_state,
//      endpoint_0_chunk_id, endpoint_1_chunk_id) stamped into the
//      payload. company_id is what PR #53's tenant-scoped delete
//      filter relies on.
//
//   5. UPSERT knowledge_edge_embeddings row with the new
//      indexed_edge_version and qdrant_point_id.
//
// The 5b-contract 'model re-enable does not backfill' (V037 header):
// a model that flips from disabled→enabled produces no historical
// events. A future bulk-resync job will emit one upsert event per
// (edge, newly-enabled model) — that job is not this worker.
//
// Retrieval safety before the worker catches up:
// EvidenceGate never trusts Qdrant for authorization — a stale or
// missing point is a recall issue, not a leak.
// ---------------------------------------------------------------------------

class EmbedEdgeWorker {
public:
    using ShutdownPredicate = std::function<bool()>;

    struct Options {
        std::chrono::milliseconds poll_interval = std::chrono::milliseconds(500);
        int max_attempts                        = 5;
        int batch_size                          = 16;
        std::chrono::minutes claim_lease        = std::chrono::minutes(10);
    };

    // Resolve the vector store bound to a given Qdrant collection. Same
    // pattern as ResyncWorker: chunk vectors live in the model's chunk
    // collection; edge points live in the deployment's edge collection.
    // The worker reads endpoint chunk vectors from the model's chunk
    // collection (looked up by embedding_models.qdrant_collection) and
    // writes edge vectors to the edge collection (fixed by config).
    using VectorStoreForCollection =
        std::function<std::shared_ptr<rag::VectorStorePort>(const std::string& collection)>;

    EmbedEdgeWorker(drogon::orm::DbClientPtr                    db,
                    VectorStoreForCollection                    store_for_collection,
                    std::shared_ptr<rag::VectorStorePort>       edge_store,
                    std::string                                 edge_collection,
                    ShutdownPredicate                           shutdown_requested,
                    Options                                     opts);

    drogon::Task<void> run();
    drogon::Task<int>  drain_once();
    drogon::Task<int>  release_my_claims();
    drogon::Task<int>  reap_stale_claims();

    std::size_t events_completed()      const { return events_completed_.load(); }
    std::size_t events_failed()         const { return events_failed_.load(); }
    std::size_t events_superseded()     const { return events_superseded_.load(); }
    std::size_t events_no_edge()        const { return events_no_edge_.load(); }
    std::size_t events_model_disabled() const { return events_model_disabled_.load(); }

private:
    struct ClaimedEvent {
        std::string  id;
        std::string  company_id;
        std::string  edge_id;
        std::string  edge_type;
        std::string  embedding_model_id;
        int          formula_version = 0;
        // edge_version and review_state are payload-time snapshots used
        // ONLY for the pre-check / staleness gate. Every payload field
        // written to Qdrant is re-read live in process() so a stale
        // event cannot durably stamp the wrong review_state.
        std::int64_t edge_version    = 0;
        std::string  review_state;
    };

    enum class Outcome { Completed, Superseded, NoEdge, ModelDisabled };

    // Live snapshot the worker takes at process() time so payload
    // fields cannot be racy vs. the enqueue moment.
    struct LiveEdge {
        std::string  edge_type;
        std::string  review_state;
        std::int64_t edge_version = 0;
        std::string  ep0_chunk_id;
        std::string  ep1_chunk_id;
        int          auth0 = 50;
        int          auth1 = 50;
        // Set to true when the SELECT returned zero rows. Kept as an
        // in-struct sentinel rather than wrapping in std::optional so
        // helper return types stay Result<void> (nested
        // std::expected<std::optional<T>, Error> combos trip GCC 14's
        // coroutine-frame emitter — see the flatten note on process()).
        bool         missing = false;
    };

    drogon::Task<std::vector<ClaimedEvent>> claim_batch();
    drogon::Task<Result<Outcome>>           process(const ClaimedEvent& ev);
    drogon::Task<void>                      mark_completed(const std::string& event_id);
    drogon::Task<void>                      mark_failed(const std::string& event_id,
                                                        std::string_view  reason);

    // Helper coroutines — all return Result<void> and write results
    // through mutable out-params. GCC 14's coroutine-frame emitter
    // ICEs on heavier return types (nested Result<optional<T>>,
    // Result<pair<...>>) even when the surrounding function is small;
    // Result<void> keeps every helper's frame minimal.
    drogon::Task<Result<void>>
    load_live_edge(const ClaimedEvent& ev, LiveEdge& out);

    drogon::Task<Result<std::int64_t>>
    load_indexed_edge_version(const ClaimedEvent& ev);

    // out_collection is set to the empty string when the model exists
    // but is disabled — caller maps that to Outcome::ModelDisabled.
    drogon::Task<Result<void>>
    load_model_collection(const std::string& model_id, std::string& out_collection);

    drogon::Task<Result<void>>
    load_endpoint_point_ids(const ClaimedEvent& ev, const LiveEdge& edge,
                            std::string& out_ep0_pid, std::string& out_ep1_pid);

    drogon::Task<Result<void>>
    load_type_vector(const ClaimedEvent& ev, rag::Embedding& out_v_type);

    // Terminal phase: read endpoint chunk vectors from Qdrant, compute
    // formula v1, upsert edge point, and update bookkeeping. Split out
    // so process() itself has a tiny frame GCC 14 can emit.
    drogon::Task<Result<Outcome>>
    write_edge_vector(const ClaimedEvent& ev,
                      const LiveEdge&     live,
                      const std::string&  collection,
                      std::shared_ptr<rag::VectorStorePort> chunk_store);

    drogon::Task<Result<void>>
    upsert_bookkeeping(const ClaimedEvent& ev,
                       const std::string&  qdrant_point_id,
                       std::int64_t        edge_version);

    drogon::orm::DbClientPtr                    db_;
    VectorStoreForCollection                    store_for_collection_;
    std::shared_ptr<rag::VectorStorePort>       edge_store_;
    std::string                                 edge_collection_;
    ShutdownPredicate                           shutdown_;
    Options                                     opts_;
    std::string                                 worker_id_;
    std::atomic<std::size_t>                    events_completed_{0};
    std::atomic<std::size_t>                    events_failed_{0};
    std::atomic<std::size_t>                    events_superseded_{0};
    std::atomic<std::size_t>                    events_no_edge_{0};
    std::atomic<std::size_t>                    events_model_disabled_{0};
};

} // namespace wikore::scheduler
