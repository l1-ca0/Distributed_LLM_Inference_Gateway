// Test 2: Gossip Failure Detection (30 pts, DR3).
//
// Verifies the SWIM protocol's core job: when a replica crashes, every
// surviving node and the gateway observe it as DEAD within a bounded time.
//
// Expected detection latency per the SWIM config used by TestCluster:
//     T_protocol (200ms) + T_ping (100ms) + k*T_ping (200ms) + T_suspect (1s)
//     ≈ 1.5 seconds under ideal conditions.
// We allow up to 10 seconds — random peer selection means the dead node may
// not be probed every round, and pessimistic slack absorbs flakes.
//
// Sub-checks:
//   (a) All 4 surviving replicas AND the gateway agree r3 is DEAD.
//   (b) Surviving replicas stay ALIVE (no collateral false positives).
//   (c) Post-failure traffic never reaches r3 (routing respects gossip).

#include "client/client.h"
#include "tests/test_cluster.h"
#include "tests/test_common.h"
#include "tests/test_runner.h"

namespace llmgateway {

TestResult test2_gossip_failure_detection() {
    return run_test("Test 2: Gossip Failure Detection", 30, [] {
        TestCluster cluster;
        for (int i = 1; i <= 5; ++i) {
            cluster.AddReplica("r" + std::to_string(i),
                               {.token_delay_ms = 10, .max_capacity = 16});
        }
        cluster.StartGateway();
        ASSERT(cluster.WaitForConvergence(8000),
               "initial convergence with 5 replicas");

        // Kill r3.
        cluster.KillReplica("r3");

        // (a) Wait for all survivors + gateway to see r3 as DEAD.
        // With 5 replicas probing randomly, the dead node may not be picked
        // every round, so allow up to 15s for full propagation.
        bool detected = wait_for([&]() {
            for (const std::string& observer : {"r1", "r2", "r4", "r5"}) {
                auto v = cluster.GetReplicaViewOfMember(observer, "r3");
                if (!v || v->state != gossip::DEAD) return false;
            }
            auto gw = cluster.GetGatewayViewOfReplica("r3");
            return gw && gw->state == gossip::DEAD;
        }, 15000);
        ASSERT(detected,
               "all 4 survivors + gateway should see r3 as DEAD within 15s");

        // (b) Survivors stabilize as ALIVE.
        // SWIM does not forbid transient SUSPECT — a healthy node may briefly
        // be suspected if an ACK lags behind the ping timeout, but must
        // refute (incarnation++) and return to ALIVE. wait_for lets that
        // refutation settle before the check fails the test.
        bool survivors_alive = wait_for([&]() {
            for (const std::string& id : {"r1", "r2", "r4", "r5"}) {
                auto gw = cluster.GetGatewayViewOfReplica(id);
                if (!gw || gw->state != gossip::ALIVE) return false;
            }
            return true;
        }, 4000);
        ASSERT(survivors_alive,
               "surviving replicas should stabilize as ALIVE (post-refute)");

        // (c) New requests only hit survivors.
        auto results = InferenceClient::InferConcurrent(
            cluster.GetGatewayAddress(), 12, "c", "post-fail-prompt", 2);
        for (const auto& r : results) {
            ASSERT(r.success, "post-failure request should succeed");
            for (const auto& rid : r.replica_ids) {
                ASSERT(rid != "r3", "no request should be routed to dead r3");
            }
        }
    });
}

}  // namespace llmgateway
