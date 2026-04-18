#pragma once

// ---------------------------------------------------------------------------
// TestCluster: in-process test infrastructure for the distributed LLM gateway.
//
// Runs multiple replicas + one gateway as C++ objects in a single process,
// communicating via localhost UDP (gossip) and TCP (gRPC). Compared to
// spawning separate processes, this approach:
//
//   + Eliminates process management overhead (no fork/exec, no PID tracking)
//   + Avoids port allocation conflicts and TIME_WAIT issues between runs
//   + Gives tests direct access to component internals (membership views,
//     circuit breaker states, active request counters) for precise asserts
//   + Makes cleanup deterministic (RAII destructors always run)
//
// The one limitation is that Test 7 (rolling update) needs to simulate
// a process restart. TestCluster handles this via RestartReplica(), which
// destructs the old in-process objects and constructs new ones on the same
// ports — functionally equivalent to a process restart for the gossip
// protocol's purposes.
//
// Usage:
//   TestCluster cluster;
//   cluster.AddReplica("r1", {.token_delay_ms = 50, .max_capacity = 4});
//   cluster.AddReplica("r2", {.token_delay_ms = 50, .max_capacity = 4});
//   cluster.AddReplica("r3", {.token_delay_ms = 50, .max_capacity = 4});
//   cluster.StartGateway();
//   cluster.WaitForConvergence();
//
//   // Run test assertions...
//   InferenceClient client(cluster.GetGatewayAddress());
//   auto result = client.Infer("test-client", "hello", 5);
//   ASSERT(result.success, "request should succeed");
//
//   cluster.StopAll();  // also happens automatically in destructor
// ---------------------------------------------------------------------------

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "gateway/circuit_breaker.h"
#include "gateway/gateway_server.h"
#include "gateway/load_balancer.h"
#include "gateway/replica_registry.h"
#include "gateway/request_queue.h"
#include "gateway/rolling_updater.h"
#include "gossip/swim.h"
#include "replica/replica_server.h"

namespace llmgateway {

// Configuration for a replica spawned by TestCluster.
struct ReplicaConfig {
    int token_delay_ms = 50;
    int max_capacity = 4;
    std::string model_version = "v1";
    double error_rate = 0.0;
    bool reject_all = false;
};

// Bundle of all objects that make up a single in-process replica.
// Each replica has:
//   - A gRPC server (ReplicaServer) for Generate/Drain RPCs on grpc_port
//   - A SWIM gossip instance on gossip_port for membership protocol
// Both are tied together via the replica's unique ID.
struct ReplicaNode {
    std::string id;
    uint16_t grpc_port;
    uint16_t gossip_port;
    ReplicaConfig config;
    std::unique_ptr<ReplicaServer> server;
    std::unique_ptr<gossip::SwimProtocol> swim;
};

// Bundle of all gateway-side objects.
struct GatewayNode {
    uint16_t grpc_port;
    uint16_t gossip_port;
    std::unique_ptr<ReplicaRegistry> registry;
    std::unique_ptr<LoadBalancer> lb;
    std::unique_ptr<CircuitBreakerManager> cb_manager;
    std::unique_ptr<RequestQueue> queue;
    std::unique_ptr<gossip::SwimProtocol> swim;
    std::unique_ptr<GatewayServer> server;
    std::unique_ptr<RollingUpdater> updater;
};

class TestCluster {
public:
    TestCluster();
    ~TestCluster();

    // Non-copyable, non-movable.
    TestCluster(const TestCluster&) = delete;
    TestCluster& operator=(const TestCluster&) = delete;

    // Add a replica and start it. Allocates two ports (gRPC + gossip) using
    // the convention grpc_port = gossip_port - 10000 so the gateway's auto-
    // derivation works.
    void AddReplica(const std::string& id, const ReplicaConfig& config = {});

    // Start the gateway. Must be called after AddReplica() calls so it can
    // seed from the existing replicas' gossip ports.
    void StartGateway();

    // Wait for the gateway to see all currently-added replicas as ALIVE.
    // Returns true on success, false on timeout.
    bool WaitForConvergence(int timeout_ms = 5000);

    // Address accessors for tests.
    std::string GetGatewayAddress() const;
    std::vector<std::string> GetReplicaIds() const;

    // Internal-state accessors for assertions.
    ReplicaServer* GetReplica(const std::string& id);
    ReplicaRegistry* GetRegistry();
    CircuitBreakerManager* GetCircuitBreakerManager();
    RequestQueue* GetRequestQueue();
    LoadBalancer* GetLoadBalancer();
    RollingUpdater* GetRollingUpdater();

    // Access the gateway's view of a specific replica's membership state.
    // Useful for tests that check gossip convergence (Test 3).
    std::optional<gossip::MemberInfo> GetGatewayViewOfReplica(
        const std::string& id);

    // Access a specific replica's view of another replica's membership state.
    // Useful for tests that verify peer-to-peer gossip state (Test 2, 3, 6).
    std::optional<gossip::MemberInfo> GetReplicaViewOfMember(
        const std::string& observer_id, const std::string& target_id);

    // Kill a replica (stops gRPC + gossip, destructs the objects).
    // Simulates a crash — the replica's ports become free but the gossip
    // network will detect it via ping timeouts.
    void KillReplica(const std::string& id);

    // Restart a previously-killed replica on the same ports with a (possibly
    // different) config. The replica rejoins the gossip network.
    void RestartReplica(const std::string& id, const ReplicaConfig& config = {});

    // Stop all replicas and the gateway in a clean order.
    // Idempotent — safe to call multiple times (destructor also calls this).
    void StopAll();

    // Number of replicas currently in the cluster (alive or killed).
    size_t NumReplicas() const;

private:
    // Collect seed addresses (gossip host:port) of all currently-alive replicas.
    // Used when a new replica joins — it needs to know at least one other
    // node's gossip address to bootstrap.
    std::vector<std::string> GetAliveSeeds(
        const std::string& exclude_id = "") const;

    // Tighter SWIM config for faster tests (200ms rounds, 1s suspect timeout).
    gossip::SwimConfig TestSwimConfig() const;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::unique_ptr<ReplicaNode>> replicas_;
    std::unique_ptr<GatewayNode> gateway_;
};

}  // namespace llmgateway
