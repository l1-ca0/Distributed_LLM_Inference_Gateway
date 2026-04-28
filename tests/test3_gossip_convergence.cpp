// Test 3: Gossip Convergence (30 pts, DR3).
//
// Tests the harder side of SWIM: simultaneous departures + a fresh joiner
// must end in a single agreed-upon membership view across the live set.
//
// Scenario:
//   1. Stable 5-replica cluster converges.
//   2. Kill r4 and r5 simultaneously.
//   3. Wait for all survivors to mark both as DEAD.
//   4. Add a new replica r6.
//   5. Require: r1, r2, r3, r6, and the gateway all agree on the live set:
//        - r4 and r5 are DEAD in the views of the pre-existing nodes
//        - r1, r2, r3, r6 are ALIVE in every live node's view
//   6. Traffic routes only to the current live set.
//
// Deliberately NOT tested: that r6 learns DEAD entries for r4/r5 via
// piggyback. The piggyback buffer evicts updates after kMaxPiggybackSends
// retransmissions (~2s of gossip), which is sooner than r6's join if the
// DEAD propagation finishes before r6 arrives — so late joiners may simply
// have no knowledge of long-gone peers. This is a standard SWIM tradeoff;
// for a live-routing gateway it doesn't matter (routing only uses the live
// set), and strictly guaranteeing full historical state to every joiner
// would require an orthogonal "full state sync on join" mechanism that
// this implementation intentionally omits.
//
// What we exercise:
//   - Dead state is sticky: simultaneous failures don't confuse pings, and
//     the stale-DEAD-ignored rule (Rule 3 in MembershipList::ApplyUpdate)
//     prevents resurrection by stale piggybacks.
//   - Piggybacked dissemination carries the new joiner's membership to all
//     pre-existing peers — r6 is visible to everyone (and vice versa for
//     the live peers visible to r6).


#include "client/client.h"
#include "tests/test_cluster.h"
#include "tests/test_common.h"
#include "tests/test_runner.h"

namespace llmgateway {

TestResult test3_gossip_convergence() {
    return run_test("Test 3: Gossip Convergence", 30, [] {
        TestCluster cluster;
        for (int i = 1; i <= 5; ++i) {
            cluster.AddReplica("r" + std::to_string(i),
                               {.token_delay_ms = 10, .max_capacity = 16});
        }
        cluster.StartGateway();
        ASSERT(cluster.WaitForConvergence(8000),
               "initial convergence with 5 replicas");

        // Kill r4 and r5 simultaneously.
        cluster.KillReplica("r4");
        cluster.KillReplica("r5");

        // Wait for all surviving replicas to mark both as DEAD.
        // Two simultaneous failures take longer to propagate than one —
        // give generous time (15s) for the full SUSPECT→DEAD pipeline.
        bool dead_everywhere = wait_for([&]() {
            for (const std::string& observer : {"r1", "r2", "r3"}) {
                for (const std::string& target : {"r4", "r5"}) {
                    auto v = cluster.GetReplicaViewOfMember(observer, target);
                    if (!v || v->state != gossip::DEAD) return false;
                }
            }
            return true;
        }, 15000);
        ASSERT(dead_everywhere,
               "r4 and r5 should be DEAD in all 3 survivors within 15s");

        // Add a fresh joiner. It will bootstrap from existing members
        // (TestCluster picks up currently-alive seeds automatically).
        cluster.AddReplica("r6", {.token_delay_ms = 10, .max_capacity = 16});

        // Full convergence: everyone agrees on the new membership.
        // Propagation has to share piggyback slots with the still-fresh DEAD
        // updates for r4/r5 (the max_piggyback_updates cap means each gossip
        // message carries at most 8 updates), so we give it 30s rather than
        // expecting instant join.
        bool converged = wait_for([&]() {
            // r6 sees the surviving cohort as ALIVE.
            for (const std::string& id : {"r1", "r2", "r3"}) {
                auto v = cluster.GetReplicaViewOfMember("r6", id);
                if (!v || v->state != gossip::ALIVE) return false;
            }
            // The survivors see r6 as ALIVE.
            for (const std::string& observer : {"r1", "r2", "r3"}) {
                auto v = cluster.GetReplicaViewOfMember(observer, "r6");
                if (!v || v->state != gossip::ALIVE) return false;
            }
            // Gateway sees r6 as ALIVE.
            auto gw = cluster.GetGatewayViewOfReplica("r6");
            if (!gw || gw->state != gossip::ALIVE) return false;
            return true;
        }, 60000);
        ASSERT(converged,
               "new joiner r6 should converge with cluster within 60s");

        // r4 / r5 must still be DEAD from the gateway's view (stale-DEAD
        // ignored rule prevents r6's join-time piggyback from resurrecting
        // them even if r6's initial self-update could collide). r6 itself
        // may never have heard of r4/r5 at all (see doc comment at top);
        // that's fine. What matters for routing is that every live member
        // recognizes r4/r5 as not-eligible.
        for (const std::string& dead : {"r4", "r5"}) {
            auto gw = cluster.GetGatewayViewOfReplica(dead);
            ASSERT(gw && gw->state == gossip::DEAD,
                   dead + " must remain DEAD in gateway view after r6 joins");
            for (const std::string& observer : {"r1", "r2", "r3"}) {
                auto v = cluster.GetReplicaViewOfMember(observer, dead);
                ASSERT(v && v->state == gossip::DEAD,
                       dead + " must remain DEAD in " + observer +
                           "'s view after r6 joins");
            }
            // r6's view: either absent or not-ALIVE (never resurrected).
            auto r6v = cluster.GetReplicaViewOfMember("r6", dead);
            if (r6v) {
                ASSERT(r6v->state != gossip::ALIVE,
                       dead + " must not be ALIVE in r6's view");
            }
        }

        // Routing verification: no traffic reaches dead replicas.
        auto results = InferenceClient::InferConcurrent(
            cluster.GetGatewayAddress(), 12, "c", "post-converge", 2);
        for (const auto& r : results) {
            ASSERT(r.success, "post-convergence request should succeed");
            for (const auto& rid : r.replica_ids) {
                ASSERT(rid != "r4" && rid != "r5",
                       "no traffic to dead replicas");
            }
        }
    });
}

}  // namespace llmgateway
