#pragma once

#include <atomic>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "gossip.pb.h"

namespace llmgateway {

// ---------------------------------------------------------------------------
// ReplicaInfo: metadata for a single LLM inference replica.
//
// Each replica is identified by a unique replica_id (e.g., "replica-1").
// The struct tracks two network addresses:
//   - gossip_address: the UDP endpoint used by the SWIM protocol for
//     failure detection (heartbeats, pings, acks).
//   - grpc_address: the TCP endpoint where the replica serves the
//     LLMReplica::Generate gRPC streaming API.
//
// Address derivation convention: the gateway derives grpc_address from
// gossip_address automatically using the rule
//     gRPC port = gossip port - 10000
// (e.g., gossip 17001 -> gRPC 7001). This avoids a separate discovery
// channel; the gossip layer is the single source of membership truth
// and gRPC addresses fall out deterministically from it.
//
// active_requests is an atomic counter updated in real-time by the gateway
// on dispatch/completion, while reported_active_requests is a stale value
// piggybacked on gossip messages from the replica itself. The load balancer
// uses the atomic counter for local routing decisions.
//
// max_capacity bounds how many concurrent requests the replica will accept.
// A value of 0 means "unlimited" (the replica self-manages its concurrency).
// ---------------------------------------------------------------------------
struct ReplicaInfo {
    std::string grpc_address;           // host:port for gRPC inference
    std::string gossip_address;         // host:port for gossip UDP
    llmgateway::gossip::MemberState gossip_state = llmgateway::gossip::ALIVE;
    double weight = 1.0;                // capacity weight for load balancing
    std::atomic<int> active_requests{0};
    int32_t reported_active_requests = 0;  // from gossip piggyback
    int32_t max_capacity = 4;
    std::string model_version;
    bool draining = false;              // set during rolling update

    // Copy constructor is required because std::atomic is not copyable.
    // Without this, containers (e.g., std::vector<ReplicaInfo>) would fail
    // to compile. We explicitly .load() the atomic for a safe snapshot.
    ReplicaInfo() = default;
    ReplicaInfo(const ReplicaInfo& other)
        : grpc_address(other.grpc_address),
          gossip_address(other.gossip_address),
          gossip_state(other.gossip_state),
          weight(other.weight),
          active_requests(other.active_requests.load()),
          reported_active_requests(other.reported_active_requests),
          max_capacity(other.max_capacity),
          model_version(other.model_version),
          draining(other.draining) {}
};

// ---------------------------------------------------------------------------
// ReplicaRegistry: thread-safe, centralized view of all known replicas.
//
// This is the single source of truth for replica metadata within the gateway
// process. It is populated/updated by the SWIM gossip layer (via callbacks)
// and queried by the LoadBalancer on every routing decision.
//
// Thread-safety model:
//   - A std::shared_mutex protects the internal map.
//   - Read-heavy paths (GetAliveReplicas, GetReplica, Increment/Decrement)
//     acquire a shared (reader) lock so they never block each other.
//   - Write paths (UpdateFromGossip, SetDraining, SetGrpcAddress) acquire
//     an exclusive (writer) lock.
//   - The active_requests counter inside ReplicaInfo is std::atomic, so
//     IncrementActive/DecrementActive only need a shared lock (they mutate
//     the atomic, not the map itself).
// ---------------------------------------------------------------------------
class ReplicaRegistry {
public:
    // Update replica state from a gossip membership update.
    // Called by the SWIM protocol callback whenever a membership event fires
    // (join, suspect, dead, etc.). Inserts a new entry if the replica_id is
    // not yet known, and derives the gRPC address from the gossip address
    // if one has not been set explicitly.
    void UpdateFromGossip(const std::string& replica_id,
                          const llmgateway::gossip::MembershipUpdate& update);

    // Return a snapshot of replicas eligible for routing.
    // Includes ALIVE and SUSPECT replicas (the load balancer may deprioritize
    // SUSPECT ones). Excludes DEAD replicas, draining replicas, entries
    // without a gRPC address, and non-replica nodes (seeds, gateways).
    std::vector<std::pair<std::string, ReplicaInfo>> GetAliveReplicas() const;

    // Get info for a specific replica. Returns nullopt if not found.
    std::optional<ReplicaInfo> GetReplica(const std::string& replica_id) const;

    // Increment/decrement the gateway-local active request counter.
    // Called by GatewayServer::Infer() on dispatch and completion. These
    // use a shared_lock because they only mutate the atomic inside the
    // already-existing ReplicaInfo, not the map structure itself.
    void IncrementActive(const std::string& replica_id);
    void DecrementActive(const std::string& replica_id);

    // Mark a replica as draining (for rolling updates). A draining replica
    // is excluded from GetAliveReplicas, so no new requests are routed to
    // it, but in-flight requests are allowed to finish.
    void SetDraining(const std::string& replica_id, bool draining);

    // Explicitly register the gRPC address for a replica, overriding the
    // automatic gossip_port-10000 derivation. Useful in test setups or
    // when the port convention does not hold.
    void SetGrpcAddress(const std::string& replica_id, const std::string& grpc_address);

    size_t Size() const;

private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, ReplicaInfo> replicas_;
};

}  // namespace llmgateway
