#pragma once

// Orchestrates rolling replica updates: drain, stop, restart, wait for
// rejoin — one replica at a time, so the cluster keeps serving traffic
// on the remaining N-1 replicas throughout the upgrade.

#include <functional>
#include <string>
#include <vector>

#include <grpcpp/grpcpp.h>
#include "llmgateway.grpc.pb.h"

#include "gateway/load_balancer.h"
#include "gateway/replica_registry.h"

namespace llmgateway {

// ---------------------------------------------------------------------------
// RollingUpdater: orchestrates zero-downtime rolling updates of replicas.
//
// A rolling update replaces each replica one at a time with a new version
// while maintaining service availability. For each replica, the sequence is:
//
//   1. DRAIN:  Mark the replica as draining in the gateway's registry
//              (no new requests routed to it) and send a Drain RPC to the
//              replica so it stops accepting new Generate calls and waits
//              for in-flight requests to complete.
//
//   2. STOP:   Invoke the stop callback (which kills the replica process
//              or stops its gRPC server + gossip). The replica leaves the
//              gossip network (detected as DEAD by peers, or via graceful
//              leave if supported).
//
//   3. RESTART: Invoke the restart callback (which starts a new replica
//               instance with the new model version on the same ports).
//               The new replica joins the gossip network and is discovered
//               by the gateway.
//
//   4. WAIT:   Wait until the gateway sees the restarted replica as ALIVE
//              and the hash ring is rebuilt.
//
// During each drain phase, the remaining replicas absorb the traffic.
// The gateway's backpressure queue handles any temporary capacity reduction.
//
// Usage (from test harness or admin tool):
//   RollingUpdater updater(registry, lb);
//   updater.Update({"r1", "r2", "r3"}, "v2",
//       [](const string& id) { /* stop replica */ },
//       [](const string& id, const string& version) { /* restart replica */ }
//   );
// ---------------------------------------------------------------------------
class RollingUpdater {
public:
    RollingUpdater(ReplicaRegistry& registry, LoadBalancer& lb);

    // Callbacks provided by the caller to control replica lifecycle.
    // StopCallback: called to stop a drained replica (kill process, stop gRPC+gossip).
    // RestartCallback: called to start a new replica with the given model version.
    using StopCallback = std::function<void(const std::string& replica_id)>;
    using RestartCallback = std::function<void(const std::string& replica_id,
                                               const std::string& new_version)>;

    // Perform a rolling update: drain, stop, restart each replica in sequence.
    // Returns true if all replicas were updated successfully.
    bool Update(const std::vector<std::string>& replica_ids,
                const std::string& new_version,
                StopCallback stop_fn,
                RestartCallback restart_fn,
                int drain_timeout_ms = 30000,
                int rejoin_timeout_ms = 10000);

    // Drain a single replica: mark draining in registry + send Drain RPC.
    // Returns true if the drain completed (all in-flight requests finished).
    bool DrainReplica(const std::string& replica_id, int timeout_ms = 30000);

    // Wait for a replica to appear as ALIVE in the registry.
    bool WaitForRejoin(const std::string& replica_id, int timeout_ms = 10000);

private:
    ReplicaRegistry& registry_;
    LoadBalancer& lb_;
};

}  // namespace llmgateway
