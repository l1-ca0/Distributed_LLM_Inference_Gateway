// Test 5: Backpressure Under Saturation (25 pts, DR5).
//
// Dispatches more concurrent requests than the cluster can serve in parallel
// and verifies the gateway queues the overflow instead of returning
// RESOURCE_EXHAUSTED. All requests must eventually succeed, and overall
// wall-time must show evidence of queuing (somewhere between pure parallel
// and pure serial execution).
//
// Cluster shape:
//   - 2 replicas, capacity=2 each  → 4 concurrent slots cluster-wide.
//   - token_delay=100ms, max_tokens=10 → each request takes ~1s to serve.
//
// With 8 concurrent requests:
//   - Serial (capacity=1):   8 * 1s = 8s.
//   - Unbounded parallel:    1s (all overlap).
//   - Correct queueing:      ~2s (two waves of 4 concurrent).
//
// Pass criteria:
//   (a) All 8 requests succeed.
//   (b) First 4 requests immediately dispatched; remaining 4 queued and
//       dispatched as capacity frees — verified via wall-time bounds
//       consistent with two waves of parallel service (not all-parallel,
//       not serial).
//   (c) Requests are dequeued/dispatched in FIFO order — verified by
//       timing each request's completion and confirming all of the first
//       wave (indices 0–3) completed before any of the second wave (4–7).

#include <chrono>
#include <thread>

#include "client/client.h"
#include "tests/test_cluster.h"
#include "tests/test_common.h"
#include "tests/test_runner.h"

namespace llmgateway {

TestResult test5_backpressure() {
    return run_test("Test 5: Backpressure Under Saturation", 25, [] {
        TestCluster cluster;
        cluster.AddReplica("r1", {.token_delay_ms = 100, .max_capacity = 2});
        cluster.AddReplica("r2", {.token_delay_ms = 100, .max_capacity = 2});
        cluster.StartGateway();
        ASSERT(cluster.WaitForConvergence(5000), "initial convergence");

        // Fire 8 concurrent requests, recording each one's completion time
        // so we can verify FIFO dispatch order directly.
        // Requests are indexed 0..7 by the order their threads are spawned.
        // FIFO dispatch means all 0..3 (first wave) should start before any
        // of 4..7 (second wave) and therefore also finish before them.
        const int N = 8;
        const int TOKENS = 10;
        std::vector<InferResult> results(N);
        std::vector<long> done_ms(N, 0);
        std::vector<std::thread> threads;
        threads.reserve(N);

        auto t0 = std::chrono::steady_clock::now();
        // Small stagger (10ms) between launches so the FIFO admission order
        // is unambiguous; without this the thread scheduler could reorder
        // the initial SelectReplica calls enough that "FIFO by index" isn't
        // meaningful.
        for (int i = 0; i < N; ++i) {
            threads.emplace_back([&, i]() {
                InferenceClient c(cluster.GetGatewayAddress());
                results[i] =
                    c.Infer("c", "bp-prompt-" + std::to_string(i), TOKENS);
                done_ms[i] =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        for (auto& t : threads) t.join();
        long elapsed_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0)
                .count();

        // (a) All 8 must succeed with exactly TOKENS tokens each.
        for (int i = 0; i < N; ++i) {
            ASSERT(results[i].success,
                   "request " + std::to_string(i) + " should succeed");
            ASSERT(results[i].tokens.size() == TOKENS,
                   "request " + std::to_string(i) + " should yield " +
                       std::to_string(TOKENS) + " tokens");
        }

        // (b) Wall-time consistent with two waves. 4 concurrent slots, each
        // wave ≈ 10 tokens * 100ms = 1s. Expect ~2s total.
        ASSERT(elapsed_ms >= 1500,
               "elapsed " + std::to_string(elapsed_ms) +
                   "ms too fast; queuing didn't serialize anything");
        ASSERT(elapsed_ms <= 5000,
               "elapsed " + std::to_string(elapsed_ms) +
                   "ms too slow; requests appear serial not queued");

        // (c) FIFO: the max completion time of requests 0..3 (first wave)
        // should be earlier than the min completion time of 4..7 (queued).
        // A small overlap tolerance (100ms) absorbs the natural jitter of
        // two concurrent streams on different replicas finishing their last
        // token nearly simultaneously.
        long first_wave_max = 0;
        long second_wave_min = std::numeric_limits<long>::max();
        for (int i = 0; i < 4; ++i) first_wave_max = std::max(first_wave_max, done_ms[i]);
        for (int i = 4; i < 8; ++i) second_wave_min = std::min(second_wave_min, done_ms[i]);
        ASSERT(first_wave_max < second_wave_min + 100,
               "FIFO violated: first-wave max-completion " +
                   std::to_string(first_wave_max) +
                   "ms >= second-wave min-completion " +
                   std::to_string(second_wave_min) +
                   "ms; queued requests completed before dispatched ones");
    });
}

}  // namespace llmgateway
