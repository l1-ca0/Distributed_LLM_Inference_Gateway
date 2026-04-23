#pragma once

#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>

#include "gateway/replica_registry.h"

namespace llmgateway {

// ---------------------------------------------------------------------------
// LoadBalancer: two-tier routing strategy for inference requests.
//
// The design is motivated by a property unique to LLM inference: the
// KV-cache. When the same prompt prefix is routed to the same replica,
// that replica can reuse cached key/value tensors and skip re-computation,
// dramatically reducing time-to-first-token.
//
// Tier 1 -- Consistent Hashing (KV-cache affinity):
//   Hash the first 64 characters of the prompt to a point on a ring.
//   Walk clockwise until a replica with available capacity is found.
//   If the prompt prefix matches a previous request, the same replica
//   is selected (cache hit). Virtual nodes spread load evenly.
//
// Tier 2 -- Weighted Least-Connections (capacity-aware fallback):
//   If every replica on the hash ring is at capacity (or the ring is
//   empty during startup), fall back to choosing the replica with the
//   lowest score = active_requests / weight. This prevents overload
//   and works without any prompt-specific state.
//
// The consistent hash ring uses virtual_nodes_ points per replica
// (default 150) to minimize key redistribution when replicas join/leave.
// The ring is rebuilt on membership changes (triggered by gossip events),
// not on every request, so SelectReplica is a fast lookup.
// ---------------------------------------------------------------------------
class LoadBalancer {
public:
    explicit LoadBalancer(ReplicaRegistry& registry, int virtual_nodes = 150);

    // Select a replica for the given prompt. Returns replica_id and gRPC address.
    // Returns nullopt if no replica has capacity (caller should queue the request).
    //
    // `excluded` lets the caller skip specific replicas — typically used by the
    // gateway's retry loop to avoid re-selecting a replica whose circuit just
    // tripped or whose stream just failed. Without this, consistent hashing
    // would keep returning the same replica for the same prompt and the retry
    // loop would exhaust attempts on a known-bad target.
    struct Selection {
        std::string replica_id;
        std::string grpc_address;
    };
    std::optional<Selection> SelectReplica(
        const std::string& prompt,
        const std::unordered_set<std::string>& excluded = {});

    // Rebuild the consistent hash ring from the current alive replicas.
    // Should be called whenever membership changes (e.g., from a gossip
    // callback). Acquires ring_mutex_ internally; safe to call concurrently
    // with SelectReplica.
    void RebuildRing();

private:
    // Tier 1: find replica via consistent hashing.
    std::optional<Selection> SelectByConsistentHash(
        const std::string& prompt,
        const std::unordered_set<std::string>& excluded);

    // Tier 2: weighted least-connections fallback.
    std::optional<Selection> SelectByLeastConnections(
        const std::unordered_set<std::string>& excluded);

    // Hash function for the consistent hash ring.
    // Uses std::hash<string> -- fast and sufficient for load distribution
    // (cryptographic strength is not needed here).
    size_t Hash(const std::string& key) const;

    ReplicaRegistry& registry_;
    int virtual_nodes_;

    // Consistent hash ring: sorted map of hash -> replica_id.
    // The std::map gives us O(log N) lower_bound for clockwise lookup.
    // Protected by its own mutex (ring rebuild is infrequent, and the
    // mutex scope does not overlap with the registry's shared_mutex).
    std::mutex ring_mutex_;
    std::map<size_t, std::string> ring_;
};

}  // namespace llmgateway
