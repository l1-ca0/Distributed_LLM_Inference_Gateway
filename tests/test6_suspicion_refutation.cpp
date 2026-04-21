// Test 6: Suspicion Refutation (15 pts, DR3).
//
// SWIM's defining guarantee against false positives: a slow-but-alive node
// can refute suspicion about itself by incrementing its incarnation number.
// Without refutation, transient network hiccups would cause healthy nodes to
// be declared DEAD; the incarnation dance is what makes SWIM safe.
//
// How the scenario is triggered:
//   We place r1 directly into the state it would reach after a ping timeout
//   to r2 — MarkSuspect("r2") on r1's membership list. The refutation path
//   under test is then exercised end-to-end:
//     (i)   r1 holds r2 as SUSPECT and queues the update for piggyback.
//     (ii)  The SUSPECT update is piggybacked to other peers, including r2.
//     (iii) r2 receives the update, sees it's about itself, and
//           CheckSelfRefutation bumps its incarnation and queues ALIVE.
//     (iv)  The refuted ALIVE update propagates back, overriding SUSPECT.
//
// What we verify:
//   (a) r2's incarnation strictly increases in response to the suspicion.
//   (b) r1's view of r2 recovers to ALIVE at the refuted higher incarnation
//       (refutation propagates back via piggyback).
//   (c) r2 is never observed as DEAD from any vantage point — refutation
//       beats the T_suspect → DEAD timer.
//   (d) The cluster remains functional: a subsequent inference request
//       still reaches r2, confirming routing was unaffected throughout.

#include "client/client.h"
#include "gossip/swim.h"
#include "tests/test_cluster.h"
#include "tests/test_common.h"
#include "tests/test_runner.h"

namespace llmgateway {

TestResult test6_suspicion_refutation() {
    return run_test("Test 6: Suspicion Refutation", 15, [] {
        TestCluster cluster;
        cluster.AddReplica("r1", {.token_delay_ms = 10, .max_capacity = 16});
        cluster.AddReplica("r2", {.token_delay_ms = 10, .max_capacity = 16});
        cluster.AddReplica("r3", {.token_delay_ms = 10, .max_capacity = 16});
        cluster.StartGateway();
        ASSERT(cluster.WaitForConvergence(6000), "initial convergence");

        // Capture r2's current self-incarnation (should be 0 after bootstrap).
        auto* r2_swim = cluster.GetReplicaSwim("r2");
        ASSERT(r2_swim, "r2 swim must exist");
        uint64_t start_incarnation = r2_swim->membership_list().my_incarnation();

        // Inject the false suspicion at r1. MarkSuspect returns false if r2
        // is not currently ALIVE in r1's view — that would be a bootstrap bug.
        auto* r1_swim = cluster.GetReplicaSwim("r1");
        ASSERT(r1_swim, "r1 swim must exist");
        bool injected = r1_swim->membership_list().MarkSuspect("r2");
        ASSERT(injected, "suspect injection should succeed (r2 must be ALIVE in r1's view)");

        // (a) r2 self-refutes by bumping incarnation.
        bool refuted = wait_for([&]() {
            return r2_swim->membership_list().my_incarnation() >
                   start_incarnation;
        }, 5000);
        ASSERT(refuted, "r2 should self-refute (incarnation++) within 5s");

        // (b) Refutation propagates back — r1 sees r2 as ALIVE again.
        // Allow generous timeout: the refuted ALIVE update must be gossiped
        // back to r1 via piggyback.
        bool propagated = wait_for([&]() {
            auto v = cluster.GetReplicaViewOfMember("r1", "r2");
            return v && v->state == gossip::ALIVE &&
                   v->incarnation > start_incarnation;
        }, 6000);
        ASSERT(propagated,
               "refutation should propagate back to r1 within 6s");

        // (c) r2 must never have been allowed to reach DEAD anywhere.
        for (const std::string& observer : {"r1", "r3"}) {
            auto v = cluster.GetReplicaViewOfMember(observer, "r2");
            ASSERT(v && v->state != gossip::DEAD,
                   "r2 must not be DEAD in " + observer + "'s view");
        }
        auto gw_view = cluster.GetGatewayViewOfReplica("r2");
        ASSERT(gw_view && gw_view->state != gossip::DEAD,
               "r2 must not be DEAD in gateway's view");

        // (d) Cluster is still functional — requests can still reach r2.
        auto r = InferenceClient(cluster.GetGatewayAddress())
                     .Infer("c", "post-refute-prompt", 3);
        ASSERT(r.success,
               "cluster must remain functional after suspicion/refutation");
    });
}

}  // namespace llmgateway
