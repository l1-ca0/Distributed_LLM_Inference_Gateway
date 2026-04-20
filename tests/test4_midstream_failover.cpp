// Test 4: Mid-Stream Failover (25 pts, DR3 + DR4).
//
// Scenario: client requests 20 tokens. Halfway through (after 10 tokens), the
// serving replica is killed. The gateway must seamlessly reroute the remainder
// to another replica using `tokens_already_generated`, so the client sees a
// single continuous stream of ≥20 tokens with the replica_id changing partway.
//
// Structure:
//   - Slow token delay (100ms) ensures there's a stable window to kill the
//     replica mid-stream before the stream naturally ends.
//   - A separate "killer" thread waits on a promise, then calls KillReplica.
//     Issuing the kill from inside the token callback would block the client
//     reader thread for several seconds (thread joins inside KillReplica),
//     causing false failover timeouts. The detached-promise pattern keeps
//     the reader free to consume the continuation stream.
//   - We capture the first-observed replica_id as the victim so we kill the
//     actual serving replica, not a guess.
//
// Pass criteria:
//   (a) Gateway detects the broken stream and re-routes seamlessly.
//   (b) Client receives ≥ 20 tokens total without error.
//   (c) replica_id field changes partway through — proves failover.
//   (d) Client does not hang or timeout (wall-time under a bounded ceiling).
//   (e) Gossip protocol independently detects and disseminates the failure
//       to all remaining replicas and the gateway.

#include <atomic>
#include <chrono>
#include <future>
#include <set>
#include <thread>

#include "client/client.h"
#include "tests/test_cluster.h"
#include "tests/test_common.h"
#include "tests/test_runner.h"

namespace llmgateway {

TestResult test4_midstream_failover() {
    return run_test("Test 4: Mid-Stream Failover", 25, [] {
        TestCluster cluster;
        cluster.AddReplica("r1", {.token_delay_ms = 100, .max_capacity = 4});
        cluster.AddReplica("r2", {.token_delay_ms = 100, .max_capacity = 4});
        cluster.AddReplica("r3", {.token_delay_ms = 100, .max_capacity = 4});
        cluster.StartGateway();
        ASSERT(cluster.WaitForConvergence(6000), "initial convergence");

        // Promise pipes the victim replica_id from the reader thread
        // (token callback) to a dedicated killer thread.
        std::promise<std::string> kill_promise;
        auto kill_future = kill_promise.get_future();
        std::atomic<bool> kill_signal_sent{false};

        std::thread killer([&]() {
            std::string victim = kill_future.get();
            if (!victim.empty()) cluster.KillReplica(victim);
        });

        // RAII guard: unblock + join the killer thread even if an ASSERT
        // further down throws. Without this the std::thread destructor
        // would call std::terminate() on an unjoined thread and crash
        // the whole test driver.
        struct KillerGuard {
            std::promise<std::string>* p;
            std::atomic<bool>* sent;
            std::thread* t;
            ~KillerGuard() {
                if (!sent->exchange(true)) p->set_value("");
                if (t->joinable()) t->join();
            }
        } kill_guard{&kill_promise, &kill_signal_sent, &killer};

        InferenceClient client(cluster.GetGatewayAddress());
        std::string first_serving;
        auto t0 = std::chrono::steady_clock::now();
        auto result = client.InferWithCallback(
            "mid-failover-client", "mid-stream-failover-prompt", 20,
            [&](const std::string& /*tok*/, const std::string& rid, int idx) {
                if (first_serving.empty()) first_serving = rid;
                if (idx >= 10 && !kill_signal_sent.exchange(true)) {
                    kill_promise.set_value(first_serving);
                }
                return true;  // keep reading
            });
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();

        // kill_guard's destructor unblocks the promise and joins the killer
        // thread on scope exit — whether normal or via an exception from
        // one of the ASSERTs below.

        // (b) Request completed without error.
        ASSERT(result.success, "failover stream should return success");
        // (c) Full token count delivered despite the kill.
        ASSERT(result.tokens.size() >= 20,
               "client should receive ≥20 tokens total, got " +
                   std::to_string(result.tokens.size()));
        // (d) No hang/timeout — 20 tokens at 100ms/token is 2s baseline plus
        // failover overhead. 15s is a comfortable ceiling; any hang would
        // blow well past this.
        ASSERT(elapsed_ms < 15000,
               "failover request took too long: " +
                   std::to_string(elapsed_ms) + "ms");
        // (c cont.) replica_id changed partway through — proves failover.
        std::set<std::string> distinct(result.replica_ids.begin(),
                                       result.replica_ids.end());
        ASSERT(distinct.size() >= 2,
               "failover should involve ≥2 distinct replicas, saw " +
                   std::to_string(distinct.size()));
        ASSERT(first_serving == result.replica_ids[0],
               "first replica_id in response should be the original serving "
               "replica (" + first_serving + "), got " + result.replica_ids[0]);

        // (e) Gossip independently detects the killed replica and
        // disseminates the failure to the gateway + all surviving replicas.
        bool gossip_detected = wait_for([&]() {
            auto gw = cluster.GetGatewayViewOfReplica(first_serving);
            if (!gw || gw->state != gossip::DEAD) return false;
            for (const std::string& id : {"r1", "r2", "r3"}) {
                if (id == first_serving) continue;
                auto v = cluster.GetReplicaViewOfMember(id, first_serving);
                if (!v || v->state != gossip::DEAD) return false;
            }
            return true;
        }, 10000);
        ASSERT(gossip_detected,
               "gossip should independently detect the killed " +
                   first_serving + " and disseminate DEAD within 10s");
    });
}

}  // namespace llmgateway
