// Integration tests for the SWIM gossip protocol.
//
// Unlike the unit tests (test_gossip_unit.cpp) which test MembershipList and
// UdpTransport in isolation, these tests launch multiple SwimProtocol instances
// as separate in-process nodes communicating over real UDP sockets on localhost.
//
// Each test verifies a different distributed behavior:
//   - Discovery: nodes with partial seed knowledge converge to a full view
//   - Failure detection: the SWIM pipeline (ping → indirect probe → suspect → dead)
//   - Rejoin: a crashed node restarts and is re-accepted into the cluster
//   - Callbacks: the gateway subscription mechanism receives correct state transitions
//
// Timing: tests use aggressive protocol parameters (200ms rounds, 100ms ping
// timeout, 1000ms suspect timeout) for fast execution. The wait_for() helper
// polls with generous timeouts (5-8s) to avoid flakiness while still catching
// real failures.

#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "gossip/swim.h"

using namespace llmgateway::gossip;

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name)                                                  \
    std::cout << "  " << #name << "... " << std::flush;            \
    try {                                                           \
        test_##name();                                              \
        std::cout << "PASS" << std::endl;                          \
        tests_passed++;                                             \
    } catch (const std::exception& e) {                            \
        std::cout << "FAIL: " << e.what() << std::endl;            \
        tests_failed++;                                             \
    }

#define ASSERT(cond, msg)                                          \
    if (!(cond)) throw std::runtime_error(std::string(msg) +       \
        " [" + __FILE__ + ":" + std::to_string(__LINE__) + "]");

// Polls a predicate until it returns true or the timeout expires.
// Uses polling instead of sleep-and-check because gossip convergence
// timing is non-deterministic (depends on random peer selection).
template <typename Pred>
bool wait_for(Pred pred, int timeout_ms, int poll_interval_ms = 100) {
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(poll_interval_ms));
    }
    return pred();
}

// ============================================================================
// Test: Epidemic discovery via piggybacked updates
//
// Topology:  n1 --seed--> n2 --seed--> n3
//            n3 --seed--> n2
//
// n1 and n3 have no direct seed knowledge of each other. n1 should learn
// about n3 through piggybacked membership updates on messages between n1↔n2
// and n2↔n3. This verifies the epidemic dissemination property of SWIM:
// membership information spreads transitively through piggybacked updates,
// not just through direct communication.
// ============================================================================

void test_three_nodes_discovery() {
    SwimConfig config;
    config.protocol_period_ms = 200;
    config.ping_timeout_ms = 200;
    config.suspect_timeout_ms = 1000;

    auto n1 = std::make_unique<SwimProtocol>("node-1", "127.0.0.1:18001", 18001, config);
    auto n2 = std::make_unique<SwimProtocol>("node-2", "127.0.0.1:18002", 18002, config);
    auto n3 = std::make_unique<SwimProtocol>("node-3", "127.0.0.1:18003", 18003, config);

    // Partial seed knowledge: n1 knows n2, n3 knows n2, n2 knows both.
    // n1 does NOT know n3 directly — must learn via gossip piggybacking.
    n1->JoinCluster({"127.0.0.1:18002"});
    n2->JoinCluster({"127.0.0.1:18001", "127.0.0.1:18003"});
    n3->JoinCluster({"127.0.0.1:18002"});

    n1->Start();
    n2->Start();
    n3->Start();

    // Wait until each node has learned the other two by their real IDs.
    auto known = [](SwimProtocol* n, const std::string& id) {
        auto info = n->membership_list().GetMember(id);
        return info && info->state == ALIVE;
    };
    bool converged = wait_for([&]() {
        return known(n1.get(), "node-2") && known(n1.get(), "node-3") &&
               known(n2.get(), "node-1") && known(n2.get(), "node-3") &&
               known(n3.get(), "node-1") && known(n3.get(), "node-2");
    }, 5000);

    ASSERT(converged, "3 nodes should discover each other within 5s");

    n1->Stop();
    n2->Stop();
    n3->Stop();
}

// ============================================================================
// Test: Full SWIM failure detection pipeline
//
// Verifies the complete detection sequence:
//   1. node-3 is stopped (simulating a crash)
//   2. A peer pings node-3 → no ACK within T_ping
//   3. Peer sends PING_REQ to k other nodes → indirect probes also fail
//   4. Peer marks node-3 as SUSPECT
//   5. After T_suspect expires with no refutation → node-3 marked DEAD
//   6. DEAD status propagates to all surviving nodes via piggybacked updates
//
// Also verifies that surviving nodes (node-1, node-2) are NOT falsely
// affected — they should remain ALIVE throughout.
// ============================================================================

void test_failure_detection() {
    SwimConfig config;
    config.protocol_period_ms = 200;
    config.ping_timeout_ms = 200;
    config.suspect_timeout_ms = 1000;

    auto n1 = std::make_unique<SwimProtocol>("node-1", "127.0.0.1:18011", 18011, config);
    auto n2 = std::make_unique<SwimProtocol>("node-2", "127.0.0.1:18012", 18012, config);
    auto n3 = std::make_unique<SwimProtocol>("node-3", "127.0.0.1:18013", 18013, config);

    // Full mesh seeds so all nodes know each other from the start.
    n1->JoinCluster({"127.0.0.1:18012", "127.0.0.1:18013"});
    n2->JoinCluster({"127.0.0.1:18011", "127.0.0.1:18013"});
    n3->JoinCluster({"127.0.0.1:18011", "127.0.0.1:18012"});

    n1->Start();
    n2->Start();
    n3->Start();

    // Wait until each node has learned the other two by their real IDs.
    auto known = [](SwimProtocol* n, const std::string& id) {
        auto info = n->membership_list().GetMember(id);
        return info && info->state == ALIVE;
    };
    bool converged = wait_for([&]() {
        return known(n1.get(), "node-2") && known(n1.get(), "node-3") &&
               known(n2.get(), "node-1") && known(n2.get(), "node-3");
    }, 5000);
    ASSERT(converged, "nodes should converge");

    // Kill node-3 — stop the protocol and destroy the instance.
    // The UDP socket is closed, so all subsequent pings will fail.
    n3->Stop();
    n3.reset();

    // Expected detection time:
    //   T_protocol (200ms) + T_ping (100ms) + k * T_ping (200ms) + T_suspect (1000ms) ≈ 1.5s
    // We give 8s to account for random peer selection (node-3 might not be
    // selected for probing in every round).
    bool detected = wait_for([&]() {
        auto info1 = n1->membership_list().GetMember("node-3");
        auto info2 = n2->membership_list().GetMember("node-3");
        return info1 && info1->state == DEAD &&
               info2 && info2->state == DEAD;
    }, 8000);

    ASSERT(detected, "node-3 should be detected as DEAD by node-1 and node-2");

    // Surviving nodes must not be declared DEAD. A transient SUSPECT is
    // acceptable; refutation recovers it.
    auto info = n1->membership_list().GetMember("node-2");
    ASSERT(info && info->state != DEAD,
           "node-2 should not be DEAD from node-1's view");

    n1->Stop();
    n2->Stop();
}

// ============================================================================
// Test: Rejoin after failure
//
// Verifies that a node can recover from DEAD state by restarting:
//   1. node-3 crashes and is detected as DEAD
//   2. node-3 restarts (new SwimProtocol instance on the same port)
//   3. node-3 rejoins by seeding from surviving nodes
//   4. Surviving nodes learn about the rejoin via piggybacked updates
//      and transition node-3 from DEAD back to ALIVE
//
// The restarted node gets a fresh incarnation number (starting from 0),
// but since DEAD was declared at the old incarnation, the new ALIVE at
// incarnation 0 from a different "generation" is accepted. In practice,
// the new instance's piggybacked self-update overrides the DEAD entry
// because it comes from a live node responding to pings.
// ============================================================================

void test_rejoin_after_failure() {
    SwimConfig config;
    config.protocol_period_ms = 200;
    config.ping_timeout_ms = 200;
    config.suspect_timeout_ms = 1000;

    auto n1 = std::make_unique<SwimProtocol>("node-1", "127.0.0.1:18021", 18021, config);
    auto n2 = std::make_unique<SwimProtocol>("node-2", "127.0.0.1:18022", 18022, config);
    auto n3 = std::make_unique<SwimProtocol>("node-3", "127.0.0.1:18023", 18023, config);

    n1->JoinCluster({"127.0.0.1:18022", "127.0.0.1:18023"});
    n2->JoinCluster({"127.0.0.1:18021", "127.0.0.1:18023"});
    n3->JoinCluster({"127.0.0.1:18021", "127.0.0.1:18022"});

    n1->Start();
    n2->Start();
    n3->Start();

    // Phase 1: wait until n1 has learned both peers by their real IDs.
    auto known = [](SwimProtocol* n, const std::string& id) {
        auto info = n->membership_list().GetMember(id);
        return info && info->state == ALIVE;
    };
    wait_for([&]() {
        return known(n1.get(), "node-2") && known(n1.get(), "node-3");
    }, 5000);

    // Phase 2: kill node-3 and wait for DEAD detection.
    n3->Stop();
    n3.reset();

    wait_for([&]() {
        auto info = n1->membership_list().GetMember("node-3");
        return info && info->state == DEAD;
    }, 8000);

    // Phase 3: restart node-3 on the same port with fresh state.
    n3 = std::make_unique<SwimProtocol>("node-3", "127.0.0.1:18023", 18023, config);
    n3->JoinCluster({"127.0.0.1:18021", "127.0.0.1:18022"});
    n3->Start();

    // Phase 4: wait for node-1 to see node-3 as ALIVE again.
    bool rejoined = wait_for([&]() {
        auto info = n1->membership_list().GetMember("node-3");
        return info && info->state == ALIVE;
    }, 8000);

    ASSERT(rejoined, "node-3 should rejoin and be seen as ALIVE by node-1");

    n1->Stop();
    n2->Stop();
    n3->Stop();
}

// ============================================================================
// Test: Membership callback mechanism
//
// The gateway subscribes to membership changes to update its routing table.
// This test verifies that callbacks fire with the correct state transitions
// when a node dies:
//   - ALIVE → SUSPECT (when ping + indirect probes fail)
//   - SUSPECT → DEAD (when suspect timeout expires)
//
// The callback must be thread-safe since it's invoked from the SWIM receiver
// thread. We protect the events vector with a mutex.
// ============================================================================

void test_membership_callback() {
    SwimConfig config;
    config.protocol_period_ms = 200;
    config.ping_timeout_ms = 200;
    config.suspect_timeout_ms = 1000;

    auto n1 = std::make_unique<SwimProtocol>("node-1", "127.0.0.1:18031", 18031, config);
    auto n2 = std::make_unique<SwimProtocol>("node-2", "127.0.0.1:18032", 18032, config);

    // Collect membership change events from node-1's perspective.
    std::vector<std::pair<std::string, MemberState>> events;
    std::mutex events_mutex;

    n1->SetMembershipCallback([&](const std::string& id, MemberState old_s, MemberState new_s) {
        std::lock_guard lock(events_mutex);
        events.emplace_back(id, new_s);
    });

    n1->JoinCluster({"127.0.0.1:18032"});
    n2->JoinCluster({"127.0.0.1:18031"});

    n1->Start();
    n2->Start();

    // Wait until n1 has learned node-2 by its real ID.
    wait_for([&]() {
        auto info = n1->membership_list().GetMember("node-2");
        return info && info->state == ALIVE;
    }, 5000);

    // Kill node-2 and wait for full detection (SUSPECT → DEAD).
    n2->Stop();
    n2.reset();

    wait_for([&]() {
        auto info = n1->membership_list().GetMember("node-2");
        return info && info->state == DEAD;
    }, 8000);

    n1->Stop();

    // Verify the callback received the expected state transitions.
    // We should see at least SUSPECT and DEAD for node-2 (in that order).
    std::lock_guard lock(events_mutex);
    bool saw_suspect = false;
    bool saw_dead = false;
    for (const auto& [id, state] : events) {
        if (id == "node-2" && state == SUSPECT) saw_suspect = true;
        if (id == "node-2" && state == DEAD) saw_dead = true;
    }

    ASSERT(saw_suspect, "should have received SUSPECT callback for node-2");
    ASSERT(saw_dead, "should have received DEAD callback for node-2");
}

// ============================================================================
// Main
// ============================================================================

int main() {
    std::cout << "=== SWIM Integration Tests ===" << std::endl;
    TEST(three_nodes_discovery);
    TEST(failure_detection);
    TEST(rejoin_after_failure);
    TEST(membership_callback);

    std::cout << std::endl << "=== Results ===" << std::endl;
    std::cout << tests_passed << " passed, " << tests_failed << " failed" << std::endl;

    return tests_failed > 0 ? 1 : 0;
}
