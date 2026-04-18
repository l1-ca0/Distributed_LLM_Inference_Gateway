// TestCluster smoke test.
//
// Verifies the test harness itself works end-to-end by exercising the same
// behavior as the existing SWIM integration tests, but through the higher-
// level TestCluster API. The tests here are intentionally similar to
// test_swim_integration.cpp so you can compare line counts and clarity.

#include "client/client.h"
#include "common/log.h"
#include "tests/test_cluster.h"
#include "tests/test_common.h"

using namespace llmgateway;
using namespace llmgateway::gossip;

TEST_COMMON_COUNTERS()

// ---------------------------------------------------------------------------
// Smoke: spin up a 3-replica cluster + gateway, verify gossip convergence.
// ---------------------------------------------------------------------------
void test_cluster_startup() {
    TestCluster cluster;
    cluster.AddReplica("r1");
    cluster.AddReplica("r2");
    cluster.AddReplica("r3");
    cluster.StartGateway();

    ASSERT(cluster.WaitForConvergence(5000),
           "all 3 replicas should converge within 5s");
    ASSERT(cluster.NumReplicas() == 3, "should have 3 replicas");
    ASSERT(cluster.GetRegistry()->GetAliveReplicas().size() == 3,
           "registry should see 3 alive replicas");
}

// ---------------------------------------------------------------------------
// Smoke: end-to-end inference through the cluster.
// ---------------------------------------------------------------------------
void test_cluster_inference() {
    TestCluster cluster;
    cluster.AddReplica("r1", {.token_delay_ms = 20});
    cluster.AddReplica("r2", {.token_delay_ms = 20});
    cluster.StartGateway();

    ASSERT(cluster.WaitForConvergence(5000), "convergence");

    InferenceClient client(cluster.GetGatewayAddress());
    auto result = client.Infer("test-client", "hello world", 5);

    ASSERT(result.success, "inference request should succeed");
    ASSERT(result.tokens.size() == 5, "should receive 5 tokens");
    ASSERT(!result.replica_ids.empty(),
           "response should identify the serving replica");
}

// ---------------------------------------------------------------------------
// Smoke: kill a replica, verify gossip detects it.
// Compare to test_swim_integration.cpp — that test uses ~45 lines of
// SwimProtocol + JoinCluster + Start/Stop. This version is ~10 lines.
// ---------------------------------------------------------------------------
void test_cluster_failure_detection() {
    TestCluster cluster;
    cluster.AddReplica("r1");
    cluster.AddReplica("r2");
    cluster.AddReplica("r3");
    cluster.StartGateway();
    ASSERT(cluster.WaitForConvergence(5000), "initial convergence");

    // Kill r3 and wait for the gateway's registry to reflect the failure.
    cluster.KillReplica("r3");

    bool detected = wait_for([&]() {
        auto info = cluster.GetGatewayViewOfReplica("r3");
        return info && info->state == DEAD;
    }, 8000);

    ASSERT(detected, "gateway should see r3 as DEAD within 8s");

    // r1 and r2 should still be alive.
    auto r1 = cluster.GetGatewayViewOfReplica("r1");
    ASSERT(r1 && r1->state == ALIVE, "r1 should still be ALIVE");
}

// ---------------------------------------------------------------------------
// Smoke: kill and restart a replica (for rolling update testing).
// ---------------------------------------------------------------------------
void test_cluster_restart_replica() {
    TestCluster cluster;
    cluster.AddReplica("r1", {.model_version = "v1"});
    cluster.AddReplica("r2", {.model_version = "v1"});
    cluster.StartGateway();
    ASSERT(cluster.WaitForConvergence(5000), "initial convergence");

    // Kill and wait for detection.
    cluster.KillReplica("r1");
    wait_for([&]() {
        auto info = cluster.GetGatewayViewOfReplica("r1");
        return info && info->state == DEAD;
    }, 8000);

    // Restart with a new version.
    cluster.RestartReplica("r1", {.model_version = "v2"});

    // Wait for r1 to rejoin as ALIVE with the new version.
    bool rejoined = wait_for([&]() {
        auto info = cluster.GetGatewayViewOfReplica("r1");
        return info && info->state == ALIVE && info->model_version == "v2";
    }, 8000);

    ASSERT(rejoined, "r1 should rejoin with version v2 within 8s");
}

int main() {
    // Suppress component logs; keep test pass/fail output clean on stdout.
    Log::SetLevel(LogLevel::INFO);

    std::cout << "=== TestCluster Smoke Tests ===" << std::endl;
    TEST(cluster_startup);
    TEST(cluster_inference);
    TEST(cluster_failure_detection);
    TEST(cluster_restart_replica);

    std::cout << std::endl << "=== Results ===" << std::endl;
    std::cout << tests_passed << " passed, " << tests_failed << " failed"
              << std::endl;

    return tests_failed > 0 ? 1 : 0;
}
