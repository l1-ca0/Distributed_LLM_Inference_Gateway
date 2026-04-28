// Test 9: Request Hedging (15 pts, DR4).
//
// Hedging (speculative execution, a.k.a. "The Tail at Scale") dispatches the
// same request to 2 replicas concurrently, keeps the first one that produces
// a token, and cancels the other. It trades extra load for lower tail latency.
//
// Cluster shape: one fast replica and one slow replica. The hedger must
// always pick the fast one because it wins the first-token race every time.
//
// Pass criteria:
//   (a) All hedged requests succeed.
//   (b) The winning replica is the fast one (the first replica_id in each
//       response corresponds to where tokens came from).
//   (c) Total wall-time is dominated by fast-path latency, not slow-path.
//       With 5 serial requests, fast (30ms/token * 10 tokens ≈ 300ms) each,
//       total ≈ 1.5–2s. Pure-slow would be 5 * 5s = 25s.
//   (d) The slow replica returns to zero in-flight requests shortly after
//       (the loser's stream was cancelled cleanly, not abandoned).

#include <chrono>
#include <set>

#include "client/client.h"
#include "tests/test_cluster.h"
#include "tests/test_common.h"
#include "tests/test_runner.h"

namespace llmgateway {

TestResult test9_request_hedging() {
    return run_test("Test 9: Request Hedging", 15, [] {
        TestCluster cluster;
        cluster.AddReplica("fast",
                           {.token_delay_ms = 30, .max_capacity = 16});
        cluster.AddReplica("slow",
                           {.token_delay_ms = 500, .max_capacity = 16});
        cluster.StartGateway();
        ASSERT(cluster.WaitForConvergence(5000), "initial convergence");

        InferenceClient client(cluster.GetGatewayAddress());

        // Send 5 hedged requests serially so we can time them.
        constexpr int N = 5;
        constexpr int TOKENS = 10;
        auto t0 = std::chrono::steady_clock::now();
        std::vector<InferResult> results;
        for (int i = 0; i < N; ++i) {
            results.push_back(client.Infer(
                "hedge-client",
                "hedge-prompt-" + std::to_string(i),
                TOKENS,
                /*hedge=*/true));
        }
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();

        // (a) All succeed with the full token count.
        for (const auto& r : results) {
            ASSERT(r.success, "hedged request should succeed");
            ASSERT(r.tokens.size() == TOKENS,
                   "hedged request should yield " + std::to_string(TOKENS) +
                       " tokens");
            ASSERT(!r.replica_ids.empty(),
                   "hedged response should identify serving replica");
        }

        // (b) The fast replica wins every race. We check the first response
        // id — subsequent ids may differ only if mid-stream failover kicks in,
        // which shouldn't happen here.
        for (const auto& r : results) {
            ASSERT(r.replica_ids[0] == "fast",
                   "fast replica should win the hedge race, got " +
                       r.replica_ids[0]);
        }

        // (c) Wall-time is fast-path dominated. Slow-replica baseline would
        // be 5 × 5s = 25s; assert well below that.
        ASSERT(elapsed_ms < 5000,
               "hedged requests wall-time " + std::to_string(elapsed_ms) +
                   "ms should be fast-path dominated (<5s for 5*10 tokens)");
        double per_request_ms = static_cast<double>(elapsed_ms) / N;
        ASSERT(per_request_ms < 1000,
               "average per-hedged-request time " +
                   std::to_string(static_cast<int>(per_request_ms)) +
                   "ms should be < 1s (fast replica dominates)");

        // (d) Slow replica's in-flight count drains to 0 within a couple
        // seconds of the last request finishing (cancellation propagates).
        auto* slow = cluster.GetReplica("slow");
        ASSERT(slow, "slow replica accessor");
        bool drained = wait_for([&]() {
            return slow->active_requests() == 0;
        }, 3000);
        ASSERT(drained,
               "slow replica should drain its cancelled streams to 0 in-flight");
    });
}

}  // namespace llmgateway
