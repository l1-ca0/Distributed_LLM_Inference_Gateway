// Test 8: Circuit Breaker (20 pts, DR3).
//
// The circuit breaker complements gossip: gossip detects binary crashes,
// the circuit breaker detects "gray failure" where a replica is alive in
// the gossip layer but returning errors at the application layer.
//
// Scenario:
//   Part 1 — trip the circuit (CLOSED → OPEN):
//     Set r3 to reject every Generate request. Gossip still sees r3 as
//     ALIVE (only inference fails). Fire a batch of requests — any that
//     hash to r3 fail, the gateway records failures, and once the error
//     rate crosses the threshold, r3's circuit opens.
//
//   Part 2 — recover via HALF_OPEN probe (OPEN → CLOSED):
//     Un-degrade r3. The circuit stays OPEN until the cooldown elapses;
//     the next request that lands on r3 becomes a HALF_OPEN probe. The
//     probe succeeds against the recovered replica, so the circuit closes
//     and normal routing resumes.
//
// Pass criteria:
//   (a) Despite r3 failing, all client requests succeed — the gateway
//       routes around the bad replica via mid-stream failover and the
//       excluded-set machinery the retry loop uses.
//   (b) r3's circuit transitions to OPEN state.
//   (c) r3 stays ALIVE in gossip throughout (no false DEAD).
//   (d) After recovery + cooldown + probes, r3's circuit returns to CLOSED.

#include <chrono>
#include <thread>

#include "client/client.h"
#include "gateway/circuit_breaker.h"
#include "tests/test_cluster.h"
#include "tests/test_common.h"
#include "tests/test_runner.h"

namespace llmgateway {

TestResult test8_circuit_breaker() {
    return run_test("Test 8: Circuit Breaker", 20, [] {
        TestCluster cluster;
        cluster.AddReplica("r1", {.token_delay_ms = 10, .max_capacity = 64});
        cluster.AddReplica("r2", {.token_delay_ms = 10, .max_capacity = 64});
        cluster.AddReplica("r3", {.token_delay_ms = 10, .max_capacity = 64});
        cluster.StartGateway();
        ASSERT(cluster.WaitForConvergence(6000), "initial convergence");

        // Part 1 — degrade r3 and send enough traffic to trip its circuit.
        cluster.GetReplica("r3")->set_reject_all(true);

        // Unique prompts spread across the ring; enough land on r3 to fill
        // the CB window (size 10) and trip the circuit.
        constexpr int kRequests = 50;
        auto results = InferenceClient::InferConcurrent(
            cluster.GetGatewayAddress(), kRequests, "c", "cb-prompt", 2);
        int ok = 0;
        for (const auto& r : results) {
            if (r.success) ok++;
        }
        ASSERT(ok == kRequests,
               "all " + std::to_string(kRequests) +
                   " requests should succeed via failover around r3, got " +
                   std::to_string(ok));

        // (b) r3's circuit should be OPEN (or HALF_OPEN if cooldown already
        // elapsed; both are acceptable progress beyond CLOSED).
        auto* cbm = cluster.GetCircuitBreakerManager();
        ASSERT(cbm, "CircuitBreakerManager should exist");
        bool tripped = wait_for([&]() {
            auto s = cbm->GetState("r3");
            return s == CircuitBreaker::State::OPEN ||
                   s == CircuitBreaker::State::HALF_OPEN;
        }, 5000);
        ASSERT(tripped, "r3's circuit should trip to OPEN (or HALF_OPEN) after failures");

        // (c) r3 still ALIVE in gossip — application-level failure must not
        // bubble up as a false membership failure.
        auto r3v = cluster.GetGatewayViewOfReplica("r3");
        ASSERT(r3v && r3v->state == gossip::ALIVE,
               "r3 should remain ALIVE in gossip despite circuit open");

        // (d) Post-trip routing: once the circuit is OPEN, subsequent
        // requests must be routed exclusively to healthy r1 or r2 — the
        // final replica_id in every response should be r1 or r2 (the
        // replica that ultimately served the request), not r3.
        auto post_trip = InferenceClient::InferConcurrent(
            cluster.GetGatewayAddress(), 10, "c", "post-trip-prompt", 2);
        for (const auto& r : post_trip) {
            ASSERT(r.success, "post-trip request should succeed");
            ASSERT(!r.replica_ids.empty(), "post-trip needs replica_id");
            // The LAST replica_id in the sequence is the one that served
            // the final tokens — it must be healthy r1 or r2.
            const std::string& final_replica = r.replica_ids.back();
            ASSERT(final_replica == "r1" || final_replica == "r2",
                   "post-trip requests must finish on a healthy replica; "
                   "got final replica " + final_replica);
        }

        // Part 2 — recover r3. Wait past the cooldown (default 5s) so the
        // next request that lands on r3 becomes a HALF_OPEN probe.
        cluster.GetReplica("r3")->set_reject_all(false);
        std::this_thread::sleep_for(std::chrono::milliseconds(5500));

        // Drive enough probes (unique prompts) that at least one hashes to
        // r3 and closes the circuit.
        bool closed = wait_for([&]() {
            for (int i = 0; i < 20; ++i) {
                InferenceClient c(cluster.GetGatewayAddress());
                c.Infer("c", "probe-" + std::to_string(i), 1);
                if (cbm->GetState("r3") == CircuitBreaker::State::CLOSED) {
                    return true;
                }
            }
            return cbm->GetState("r3") == CircuitBreaker::State::CLOSED;
        }, 15000);
        ASSERT(closed, "r3's circuit should return to CLOSED after recovery");
    });
}

}  // namespace llmgateway
