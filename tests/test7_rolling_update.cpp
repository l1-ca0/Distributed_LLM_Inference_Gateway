// Test 7: Rolling Update via Gossip (15 pts, DR3 + DR6).
//
// A rolling update swaps each replica from v1 → v2 one at a time while
// continuous client traffic runs. The RollingUpdater orchestrates:
//   drain   — mark the target non-routable, then send the Drain RPC which
//             blocks until in-flight requests finish.
//   stop    — shut the replica down (in-flight count is already zero).
//   restart — spawn a new instance with the new version on the same ports.
//   rejoin  — wait for gossip to see it ALIVE and the hash ring to rebuild.
//
// The drain step is what makes "zero dropped requests" achievable:
// SetDraining(true) + RebuildRing removes the target from the ring before
// the Drain RPC blocks on in-flight traffic, so new requests are routed
// elsewhere and stop-fn runs only after existing ones finish.
//
// What we verify:
//   (a) Update completes end-to-end (RollingUpdater returns success).
//   (b) After the update, every replica reports model_version="v2" via gossip.
//   (c) The background load generator sees zero failed requests across the
//       entire update — 100% success rate.

#include <atomic>
#include <chrono>
#include <thread>

#include "client/client.h"
#include "tests/test_cluster.h"
#include "tests/test_common.h"
#include "tests/test_runner.h"

namespace llmgateway {

TestResult test7_rolling_update() {
    return run_test("Test 7: Rolling Update via Gossip", 15, [] {
        TestCluster cluster;
        auto base = ReplicaConfig{.token_delay_ms = 10,
                                  .max_capacity = 16,
                                  .model_version = "v1"};
        cluster.AddReplica("r1", base);
        cluster.AddReplica("r2", base);
        cluster.AddReplica("r3", base);
        cluster.StartGateway();
        ASSERT(cluster.WaitForConvergence(6000), "initial convergence");

        // Background load.
        std::atomic<bool> stop_load{false};
        std::atomic<int> successes{0};
        std::atomic<int> failures{0};
        std::thread loader([&]() {
            InferenceClient c(cluster.GetGatewayAddress());
            int i = 0;
            while (!stop_load.load()) {
                auto r = c.Infer("loader", "load-" + std::to_string(i++), 2);
                if (r.success) successes++;
                else failures++;
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
            }
        });

        // RAII guard: stop + join the loader even if an ASSERT throws.
        // Without this, the std::thread destructor would call
        // std::terminate() on an unjoined loader and crash the test driver.
        struct LoaderGuard {
            std::atomic<bool>* stop;
            std::thread* t;
            ~LoaderGuard() {
                stop->store(true);
                if (t->joinable()) t->join();
            }
        } loader_guard{&stop_load, &loader};

        // Let the loader build up in-flight traffic before the update starts.
        std::this_thread::sleep_for(std::chrono::milliseconds(400));

        auto* updater = cluster.GetRollingUpdater();
        ASSERT(updater, "RollingUpdater should be available");

        bool ok = updater->Update(
            {"r1", "r2", "r3"}, "v2",
            [&](const std::string& id) { cluster.KillReplica(id); },
            [&](const std::string& id, const std::string& new_ver) {
                cluster.RestartReplica(id, {.token_delay_ms = 10,
                                            .max_capacity = 16,
                                            .model_version = new_ver});
            });
        ASSERT(ok, "rolling update should report success");

        // Let load settle, then stop the generator so downstream ASSERTs
        // see the final success/failure counters. loader_guard's destructor
        // is a no-op in this path (loader.joinable() is false after join).
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        stop_load.store(true);
        loader.join();

        // (b) All replicas advertising v2 via gossip.
        bool all_v2 = wait_for([&]() {
            for (const std::string& id : {"r1", "r2", "r3"}) {
                auto info = cluster.GetGatewayViewOfReplica(id);
                if (!info || info->state != gossip::ALIVE ||
                    info->model_version != "v2") {
                    return false;
                }
            }
            return true;
        }, 8000);
        ASSERT(all_v2, "all 3 replicas should be ALIVE on v2 within 8s");

        // (c) Zero failures: 100% success rate is the core availability
        // guarantee of a rolling update. The drain step (SetDraining +
        // RebuildRing) ensures no new requests dispatch to a target before
        // the Drain RPC blocks waiting for in-flight ones, so no request
        // is ever sent to a replica that is about to stop.
        int total = successes.load() + failures.load();
        ASSERT(total >= 10,
               "loader should have issued meaningful traffic, got " +
                   std::to_string(total));
        ASSERT(failures.load() == 0,
               "rolling update must not drop any requests; saw " +
                   std::to_string(failures.load()) + " failures out of " +
                   std::to_string(total));
    });
}

}  // namespace llmgateway
