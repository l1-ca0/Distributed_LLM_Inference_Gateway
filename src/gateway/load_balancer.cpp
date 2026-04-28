#include "gateway/load_balancer.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <mutex>
#include <random>

#include "common/log.h"

namespace llmgateway {

// Thread-local PRNG for reservoir-sampling tiebreak in least-connections.
// Using thread_local avoids mutex contention on the RNG across gateway
// request threads.
static thread_local std::mt19937 g_rng{std::random_device{}()};

LoadBalancer::LoadBalancer(ReplicaRegistry& registry, int virtual_nodes)
    : registry_(registry), virtual_nodes_(virtual_nodes) {}

// SelectReplica: the hot path called on every incoming inference request.
// It tries Tier 1 first (consistent hash for cache affinity) and only
// falls through to Tier 2 (least-connections) if the ring is empty or
// every ring position is at capacity.
std::optional<LoadBalancer::Selection> LoadBalancer::SelectReplica(
    const std::string& prompt,
    const std::unordered_set<std::string>& excluded) {

    // Tier 1: try consistent hashing for prompt-prefix affinity.
    auto selection = SelectByConsistentHash(prompt, excluded);
    if (selection) return selection;

    // Tier 2: fall back to weighted least-connections.
    return SelectByLeastConnections(excluded);
}

// ---------------------------------------------------------------------------
// RebuildRing: reconstruct the hash ring from the current alive replica set.
//
// Called on membership changes (replica join, leave, or state transition).
// We snapshot the alive replicas first (shared lock on the registry),
// then take the ring mutex to swap in the new ring. This two-step approach
// avoids holding both locks simultaneously.
//
// Virtual nodes: each physical replica is placed at virtual_nodes_ positions
// on the ring using hash(replica_id + "#" + i). This ensures that when a
// single replica leaves, only ~1/N of the key space is remapped (consistent
// hashing property), and the load is spread evenly rather than clumped
// around a few hash positions.
// ---------------------------------------------------------------------------
void LoadBalancer::RebuildRing() {
    auto alive = registry_.GetAliveReplicas();

    std::lock_guard lock(ring_mutex_);
    ring_.clear();

    for (const auto& [id, info] : alive) {
        for (int i = 0; i < virtual_nodes_; i++) {
            std::string key = id + "#" + std::to_string(i);
            size_t h = Hash(key);
            ring_[h] = id;
        }
    }

    LOG_DEBUG("lb", "ring rebuilt: %zu replicas, %zu points",
              alive.size(), ring_.size());
}

// ---------------------------------------------------------------------------
// Tier 1: Consistent hashing with clockwise walk.
//
// 1. Hash the prompt prefix (first 64 chars). Using only the prefix means
//    requests that share a common prefix (e.g., system prompt + few-shot
//    examples) will hash to the same ring position, giving stable prefix
//    affinity.
//
// 2. lower_bound(h) finds the first ring point >= h (clockwise from h).
//    If we overshoot the end, wrap to ring_.begin() (the ring is circular).
//
// 3. Walk clockwise, checking each candidate replica for:
//      - not draining (rolling update in progress)
//      - not DEAD (gossip has confirmed failure)
//      - has capacity: max_capacity == 0 means unlimited (the replica
//        self-manages concurrency), otherwise active_requests < max_capacity
//    The first replica satisfying all three is returned.
//
// 4. If the walk completes a full loop without finding capacity, return
//    nullopt so the caller falls through to Tier 2.
// ---------------------------------------------------------------------------
std::optional<LoadBalancer::Selection> LoadBalancer::SelectByConsistentHash(
    const std::string& prompt,
    const std::unordered_set<std::string>& excluded) {

    std::lock_guard lock(ring_mutex_);
    if (ring_.empty()) return std::nullopt;

    // Hash the prompt prefix (first 64 chars or the whole prompt if shorter).
    std::string prefix = prompt.substr(0, std::min<size_t>(64, prompt.size()));
    size_t h = Hash(prefix);

    // Find the first ring position >= h (clockwise walk).
    auto it = ring_.lower_bound(h);
    if (it == ring_.end()) it = ring_.begin();  // wrap around

    // Walk the ring until we find a replica with capacity.
    size_t start_hash = it->first;
    bool wrapped = false;

    while (true) {
        const auto& replica_id = it->second;

        // Skip replicas the caller has asked us to exclude (e.g., circuits
        // open, just-failed stream). The next ring point may be a different
        // replica; if not, we'll walk past it on the next iteration.
        if (excluded.count(replica_id) == 0) {
            auto info = registry_.GetReplica(replica_id);
            // max_capacity == 0 means "unlimited": the replica has no gateway-enforced
            // concurrency cap and self-manages its own parallelism.
            if (info && !info->draining &&
                info->gossip_state != llmgateway::gossip::DEAD &&
                (info->max_capacity == 0 || info->active_requests.load() < info->max_capacity)) {
                return Selection{replica_id, info->grpc_address};
            }
        }

        // Move clockwise.
        ++it;
        if (it == ring_.end()) {
            it = ring_.begin();
            wrapped = true;
        }

        // Full loop -- no capacity anywhere on the ring.
        if (wrapped && it->first >= start_hash) break;
    }

    return std::nullopt;  // Ring exhausted, fall through to Tier 2.
}

// ---------------------------------------------------------------------------
// Tier 2: Weighted least-connections.
//
// Scoring: score(r) = active_requests(r) / weight(r).
//   Dividing by weight means a replica with weight=2.0 can handle twice
//   the load of a weight=1.0 replica before its score equals theirs.
//   This naturally favors higher-capacity replicas (e.g., GPU vs CPU).
//
// Capacity check: replicas at max_capacity are skipped entirely.
//   max_capacity == 0 is treated as "unlimited" (same as in Tier 1).
//
// Tiebreak: when multiple replicas have the same minimum score, we use
//   reservoir sampling (Vitter's Algorithm R) to select uniformly at random
//   among them in a single pass. This avoids the need to collect all tied
//   candidates into a separate list. The k-th tied candidate replaces the
//   current best with probability 1/k, which yields uniform selection.
//
// Returns nullopt only when every alive replica is at capacity -- the
// caller (Infer) will then return RESOURCE_EXHAUSTED to the client.
// ---------------------------------------------------------------------------
std::optional<LoadBalancer::Selection> LoadBalancer::SelectByLeastConnections(
    const std::unordered_set<std::string>& excluded) {
    auto alive = registry_.GetAliveReplicas();
    if (alive.empty()) return std::nullopt;

    std::string best_id;
    std::string best_addr;
    double best_score = std::numeric_limits<double>::max();
    int tie_count = 0;

    for (const auto& [id, info] : alive) {
        if (excluded.count(id)) continue;
        // Skip replicas that are at capacity (max_capacity > 0 means enforced).
        if (info.max_capacity > 0 && info.active_requests.load() >= info.max_capacity) continue;

        double score = static_cast<double>(info.active_requests.load()) / info.weight;

        if (score < best_score) {
            best_score = score;
            best_id = id;
            best_addr = info.grpc_address;
            tie_count = 1;
        } else if (score == best_score) {
            // Reservoir sampling: the k-th tied candidate replaces the
            // current winner with probability 1/k, guaranteeing uniform
            // random selection across all tied candidates.
            tie_count++;
            std::uniform_int_distribution<int> dist(1, tie_count);
            if (dist(g_rng) == 1) {
                best_id = id;
                best_addr = info.grpc_address;
            }
        }
    }

    if (best_id.empty()) return std::nullopt;
    return Selection{best_id, best_addr};
}

size_t LoadBalancer::Hash(const std::string& key) const {
    return std::hash<std::string>{}(key);
}

}  // namespace llmgateway
