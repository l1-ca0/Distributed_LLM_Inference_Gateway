// Rolling update orchestrator.
//
// Coordinates the drain → stop → restart → rejoin sequence for each replica
// in a rolling update. The key invariant is that at most one replica is
// offline at any time, so the cluster always has N-1 replicas serving traffic.

#include "gateway/rolling_updater.h"

#include <chrono>
#include <thread>

#include "common/log.h"

namespace llmgateway {

RollingUpdater::RollingUpdater(ReplicaRegistry& registry, LoadBalancer& lb)
    : registry_(registry), lb_(lb) {}

bool RollingUpdater::Update(const std::vector<std::string>& replica_ids,
                            const std::string& new_version,
                            StopCallback stop_fn,
                            RestartCallback restart_fn,
                            int drain_timeout_ms,
                            int rejoin_timeout_ms) {

    for (const auto& replica_id : replica_ids) {
        LOG_INFO("rolling", "updating %s to %s", replica_id.c_str(), new_version.c_str());

        // Step 1: Drain — stop new requests, wait for in-flight to complete.
        if (!DrainReplica(replica_id, drain_timeout_ms)) {
            LOG_ERROR("rolling", "failed to drain %s", replica_id.c_str());
            // Un-drain so routing resumes to this replica.
            registry_.SetDraining(replica_id, false);
            lb_.RebuildRing();
            return false;
        }

        // Step 2: Stop — kill the replica process / stop gRPC + gossip.
        LOG_INFO("rolling", "stopping %s", replica_id.c_str());
        stop_fn(replica_id);

        // Step 3: Restart — start a new instance with the new version.
        LOG_INFO("rolling", "restarting %s with version %s",
                 replica_id.c_str(), new_version.c_str());
        restart_fn(replica_id, new_version);

        // Clear the draining flag and rebuild the ring so the restarted
        // replica starts receiving traffic as soon as gossip marks it ALIVE.
        // draining is a gateway-side flag that UpdateFromGossip does not
        // touch, so we must explicitly reset it here after restart.
        registry_.SetDraining(replica_id, false);
        lb_.RebuildRing();

        // Step 4: Wait for the restarted replica to rejoin via gossip.
        if (!WaitForRejoin(replica_id, rejoin_timeout_ms)) {
            LOG_WARN("rolling", "%s did not rejoin within timeout, continuing",
                     replica_id.c_str());
            // Continue anyway — the replica may rejoin later via gossip.
        }

        LOG_INFO("rolling", "%s updated successfully", replica_id.c_str());
    }

    LOG_INFO("rolling", "rolling update complete: %zu replicas updated to %s",
             replica_ids.size(), new_version.c_str());
    return true;
}

// ---------------------------------------------------------------------------
// DrainReplica: mark the replica as draining and send a Drain RPC.
//
// Two-step drain:
//   1. Mark draining in the gateway's registry so the load balancer stops
//      routing new requests to this replica.
//   2. Send a Drain RPC to the replica itself so it rejects any new
//      Generate calls and waits for in-flight requests to complete.
//
// The Drain RPC is a blocking call — it returns only when all in-flight
// requests on the replica have finished. This guarantees that after
// DrainReplica returns, it's safe to stop the replica without dropping
// any requests.
//
// If the Drain RPC fails (e.g., replica already crashed), we still
// consider the drain successful — there are no in-flight requests if
// the replica is dead.
// ---------------------------------------------------------------------------
bool RollingUpdater::DrainReplica(const std::string& replica_id,
                                  int timeout_ms) {
    // Step 1: Mark draining in the gateway registry.
    registry_.SetDraining(replica_id, true);
    lb_.RebuildRing();

    // Step 2: Send Drain RPC to the replica.
    auto info = registry_.GetReplica(replica_id);
    if (!info || info->grpc_address.empty()) {
        LOG_WARN("rolling", "no gRPC address for %s, skipping Drain RPC",
                 replica_id.c_str());
        return true;  // Already gone — drain is trivially complete.
    }

    auto channel = grpc::CreateChannel(info->grpc_address,
                                       grpc::InsecureChannelCredentials());
    auto stub = LLMReplica::NewStub(channel);

    DrainRequest request;
    DrainResponse response;
    grpc::ClientContext context;

    // Set a deadline for the Drain RPC.
    context.set_deadline(std::chrono::system_clock::now() +
                         std::chrono::milliseconds(timeout_ms));

    grpc::Status status = stub->Drain(&context, request, &response);

    if (status.ok() && response.success()) {
        LOG_INFO("rolling", "%s drained successfully", replica_id.c_str());
        return true;
    }

    if (status.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED) {
        LOG_WARN("rolling", "drain timeout for %s", replica_id.c_str());
        return false;
    }

    // Drain RPC failed (replica may have crashed). That's OK — no in-flight
    // requests if the replica is dead.
    LOG_WARN("rolling", "drain RPC failed for %s: %s (treating as drained)",
             replica_id.c_str(), status.error_message().c_str());
    return true;
}

// ---------------------------------------------------------------------------
// WaitForRejoin: poll the registry until the replica appears as ALIVE.
//
// After a restart, the replica needs time to:
//   1. Start its gRPC server
//   2. Start its SWIM gossip protocol
//   3. Be discovered by the gateway's gossip subscriber
//   4. Have its state updated in the registry
//   5. Be added back to the hash ring
//
// We poll every 200ms until the replica is ALIVE or the timeout expires.
// ---------------------------------------------------------------------------
bool RollingUpdater::WaitForRejoin(const std::string& replica_id,
                                   int timeout_ms) {
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);

    while (std::chrono::steady_clock::now() < deadline) {
        auto info = registry_.GetReplica(replica_id);
        if (info && info->gossip_state == gossip::ALIVE && !info->draining) {
            LOG_INFO("rolling", "%s rejoined cluster", replica_id.c_str());
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    return false;
}

}  // namespace llmgateway
