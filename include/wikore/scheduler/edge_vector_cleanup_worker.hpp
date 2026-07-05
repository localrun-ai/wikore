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
// EdgeVectorCleanupWorker - drains `qdrant_delete_edge_points` events from
// outbox_events. These are enqueued by V034's BEFORE DELETE trigger on
// knowledge_edges, which captures every affected qdrant_point_id (from the
// about-to-be-cascaded knowledge_edge_embeddings rows) into the outbox
// payload BEFORE the CASCADE fires. That is the only place these ids can be
// captured: after the parent DELETE the child rows are gone.
//
// Payload shape (V034 job_schema_version = 1):
//
//   {
//     "edge_id":          "<uuid>",
//     "qdrant_point_ids": ["<uuid>", "<uuid>", ...]
//   }
//
// The trigger already guards the enqueue with EXISTS(companies), so a
// tenant offboarding (`DELETE FROM companies`) never enqueues events with a
// dangling company_id. Retrieval remains safe in the interim window before
// this worker runs because EvidenceGate always revalidates against live PG
// state — a stale point in Qdrant can be a recall issue, not a leak.
//
// Multi-model collections: V034's payload does not yet carry the collection
// name per point. Today edge embeddings are single-collection (one edge
// Qdrant collection per deployment); when per-model edge collections land
// (BaryGraph Lite implementation step 5), the payload will be enriched and
// this worker will route per collection.
// ---------------------------------------------------------------------------

class EdgeVectorCleanupWorker {
public:
    using ShutdownPredicate = std::function<bool()>;

    struct Options {
        std::chrono::milliseconds poll_interval = std::chrono::milliseconds(500);
        int max_attempts                        = 5;
        int batch_size                          = 16;
        // Stale-claim reaper lease: an event claimed longer than this is
        // presumed abandoned (worker crashed) and is reclaimed on the next
        // poll. Must exceed the worst-case per-event processing time.
        std::chrono::minutes claim_lease        = std::chrono::minutes(10);
    };

    EdgeVectorCleanupWorker(drogon::orm::DbClientPtr              db,
                            std::shared_ptr<rag::VectorStorePort> vector_store,
                            ShutdownPredicate                     shutdown_requested,
                            Options                               opts);

    drogon::Task<void> run();
    drogon::Task<int>  drain_once();

    // Release any events still claimed by THIS worker back to the unclaimed
    // pool, decrementing attempt_count so a graceful shutdown does not
    // consume the retry budget. Called automatically at the end of run().
    drogon::Task<int>  release_my_claims();

    // Reclaim events whose claimed_at is older than opts_.claim_lease
    // (presumed abandoned by a crashed worker). Idempotent; returns the
    // count reclaimed.
    drogon::Task<int>  reap_stale_claims();

    std::size_t events_completed() const { return events_completed_.load(); }
    std::size_t events_failed()    const { return events_failed_.load(); }

private:
    struct ClaimedEvent {
        std::string              id;
        std::string              company_id;
        std::string              edge_id;   // aggregate_id
        std::vector<std::string> point_ids;
    };

    drogon::Task<std::vector<ClaimedEvent>> claim_batch();
    drogon::Task<Result<void>>              process(const ClaimedEvent& ev);
    drogon::Task<void>                      mark_completed(const std::string& event_id);
    drogon::Task<void>                      mark_failed(const std::string& event_id,
                                                        std::string_view  reason);

    drogon::orm::DbClientPtr              db_;
    std::shared_ptr<rag::VectorStorePort> vector_store_;
    ShutdownPredicate                     shutdown_;
    Options                               opts_;
    std::string                           worker_id_;
    std::atomic<std::size_t>              events_completed_{0};
    std::atomic<std::size_t>              events_failed_{0};
};

} // namespace wikore::scheduler
