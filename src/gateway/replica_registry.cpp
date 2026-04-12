#include "gateway/replica_registry.h"

#include "common/log.h"

namespace llmgateway {

// ---------------------------------------------------------------------------
// UpdateFromGossip: the main entry point for integrating gossip events into
// the gateway's routing state.
//
// The SWIM gossip layer on the gateway node periodically exchanges membership
// information with replicas. Each update carries the replica's gossip address,
// current state (ALIVE/SUSPECT/DEAD), self-reported load metrics (active
// requests, max capacity), and model version.
//
// Using replicas_[replica_id] (operator[]) intentionally: if the replica_id
// is not yet in the map, this creates a default-constructed entry, which is
// exactly the "first time we hear about this replica" case.
// ---------------------------------------------------------------------------
void ReplicaRegistry::UpdateFromGossip(
    const std::string& replica_id,
    const llmgateway::gossip::MembershipUpdate& update) {

    std::unique_lock lock(mutex_);
    auto& info = replicas_[replica_id];

    info.gossip_address = update.address();
    info.gossip_state = update.state();
    info.reported_active_requests = update.active_requests();
    info.max_capacity = update.max_capacity();
    if (!update.model_version().empty()) {
        info.model_version = update.model_version();
    }

    // Derive gRPC address from gossip address if not set explicitly.
    // Convention: gRPC port = gossip port - 10000 (e.g., gossip 17001 -> gRPC 7001).
    // This avoids the need for a separate service-discovery mechanism: replicas
    // only need to advertise their gossip (UDP) port, and the gateway
    // deterministically computes the inference (gRPC/TCP) port from it.
    // This can be overridden by SetGrpcAddress() for non-standard deployments.
    if (info.grpc_address.empty() && !info.gossip_address.empty()) {
        auto colon = info.gossip_address.rfind(':');
        if (colon != std::string::npos) {
            int gossip_port = std::stoi(info.gossip_address.substr(colon + 1));
            int grpc_port = gossip_port - 10000;
            info.grpc_address = info.gossip_address.substr(0, colon) + ":" +
                                std::to_string(grpc_port);
        }
    }
}

// ---------------------------------------------------------------------------
// GetAliveReplicas: returns a snapshot of replicas eligible for load balancing.
//
// Filtering logic (every condition must pass for inclusion):
//   1. gossip_state != DEAD  -- DEAD replicas are confirmed down; SUSPECT
//      replicas are included because they may still be reachable (the load
//      balancer may deprioritize them but does not exclude them).
//   2. !draining  -- replicas marked for rolling update drain are excluded
//      so no new requests are sent to them.
//   3. !grpc_address.empty()  -- we need a valid gRPC endpoint to route to.
//      If the gossip-derived address hasn't been computed yet (e.g., the
//      first gossip message had no address), skip the entry.
//   4. id doesn't start with "seed-"  -- "seed-" entries are bootstrap
//      placeholders used by SWIM to join the cluster. They represent
//      initial contact addresses, not actual LLM replicas.
//   5. id doesn't start with "gateway"  -- gateway nodes participate in
//      gossip for cluster awareness but do not serve inference requests.
//      Without this filter, the gateway could try to route to itself.
//
// Returns a value copy (vector of pairs) so callers get a consistent
// snapshot without holding the lock during routing decisions.
// ---------------------------------------------------------------------------
std::vector<std::pair<std::string, ReplicaInfo>>
ReplicaRegistry::GetAliveReplicas() const {
    std::shared_lock lock(mutex_);
    std::vector<std::pair<std::string, ReplicaInfo>> result;

    for (const auto& [id, info] : replicas_) {
        if (info.gossip_state != llmgateway::gossip::DEAD && !info.draining &&
            !info.grpc_address.empty() &&
            id.find("seed-") != 0 && id.find("gateway") != 0) {
            result.emplace_back(id, info);
        }
    }

    return result;
}

std::optional<ReplicaInfo> ReplicaRegistry::GetReplica(
    const std::string& replica_id) const {
    std::shared_lock lock(mutex_);
    auto it = replicas_.find(replica_id);
    if (it == replicas_.end()) return std::nullopt;
    return it->second;
}

// IncrementActive / DecrementActive: gateway-local request tracking.
//
// These only need a shared_lock (not unique_lock) because they do not
// modify the map structure -- they only atomically update an int inside
// an existing ReplicaInfo. Multiple gateway threads can safely adjust
// different replicas' counters concurrently without blocking each other,
// or even the same replica's counter (since it is std::atomic<int>).
void ReplicaRegistry::IncrementActive(const std::string& replica_id) {
    std::shared_lock lock(mutex_);
    auto it = replicas_.find(replica_id);
    if (it != replicas_.end()) {
        it->second.active_requests.fetch_add(1);
    }
}

void ReplicaRegistry::DecrementActive(const std::string& replica_id) {
    std::shared_lock lock(mutex_);
    auto it = replicas_.find(replica_id);
    if (it != replicas_.end()) {
        it->second.active_requests.fetch_sub(1);
    }
}

void ReplicaRegistry::SetDraining(const std::string& replica_id, bool draining) {
    std::unique_lock lock(mutex_);
    auto it = replicas_.find(replica_id);
    if (it != replicas_.end()) {
        it->second.draining = draining;
        LOG_INFO("registry", "%s draining=%s", replica_id.c_str(),
                 draining ? "true" : "false");
    }
}

void ReplicaRegistry::SetGrpcAddress(const std::string& replica_id,
                                     const std::string& grpc_address) {
    std::unique_lock lock(mutex_);
    replicas_[replica_id].grpc_address = grpc_address;
}

size_t ReplicaRegistry::Size() const {
    std::shared_lock lock(mutex_);
    return replicas_.size();
}

}  // namespace llmgateway
