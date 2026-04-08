// Unit tests for the gossip module's foundational components:
//   - MembershipList: thread-safe membership state with SWIM incarnation-based
//     conflict resolution, piggyback buffer, and state change callbacks.
//   - UdpTransport: UDP send/receive with Protobuf serialization.

#include <cassert>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

#include "gossip/membership_list.h"
#include "gossip/udp_transport.h"

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

// ============================================================================
// MembershipList tests
//
// The MembershipList is the core data structure for SWIM gossip. It stores
// per-member state (Alive/Suspect/Dead) with incarnation-based conflict
// resolution. These tests verify rules in the SWIM conflict resolution logic.
// ============================================================================

// Basic add/get: verify members can be added and retrieved with correct state.
void test_add_and_get_member() {
    MembershipList ml("self");
    ml.AddMember("node-1", "127.0.0.1:7001");
    ml.AddMember("node-2", "127.0.0.1:7002");

    auto info = ml.GetMember("node-1");
    ASSERT(info.has_value(), "node-1 should exist");
    ASSERT(info->state == ALIVE, "node-1 should be ALIVE");
    ASSERT(info->address == "127.0.0.1:7001", "wrong address");
    ASSERT(ml.Size() == 2, "expected 2 members");
}

// GetAliveMembers() should exclude "self" — the gateway/load balancer should
// never route requests to itself.
void test_get_alive_excludes_self() {
    MembershipList ml("self");
    ml.AddMember("self", "127.0.0.1:7000");
    ml.AddMember("node-1", "127.0.0.1:7001");

    auto alive = ml.GetAliveMembers();
    ASSERT(alive.size() == 1, "should exclude self from alive list");
    ASSERT(alive[0].first == "node-1", "wrong member");
}

// SWIM Rule: higher incarnation always wins, regardless of state.
// A SUSPECT update with incarnation 5 should override an ALIVE at incarnation 0.
void test_incarnation_higher_wins() {
    MembershipList ml("self");
    ml.AddMember("node-1", "127.0.0.1:7001");  // incarnation 0, ALIVE

    MembershipUpdate update;
    update.set_member_id("node-1");
    update.set_address("127.0.0.1:7001");
    update.set_state(SUSPECT);
    update.set_incarnation(5);

    bool changed = ml.ApplyUpdate(update);
    ASSERT(changed, "higher incarnation SUSPECT should be accepted");

    auto info = ml.GetMember("node-1");
    ASSERT(info->state == SUSPECT, "should be SUSPECT");
    ASSERT(info->incarnation == 5, "incarnation should be 5");
}

// SWIM Rule: stale updates (lower incarnation) must be ignored.
// This prevents old gossip messages from reverting newer state.
void test_incarnation_lower_ignored() {
    MembershipList ml("self");

    // Set node-1 at incarnation 10.
    MembershipUpdate u1;
    u1.set_member_id("node-1");
    u1.set_address("127.0.0.1:7001");
    u1.set_state(ALIVE);
    u1.set_incarnation(10);
    ml.ApplyUpdate(u1);

    // Try to update with lower incarnation — should be ignored.
    MembershipUpdate u2;
    u2.set_member_id("node-1");
    u2.set_address("127.0.0.1:7001");
    u2.set_state(SUSPECT);
    u2.set_incarnation(5);
    bool changed = ml.ApplyUpdate(u2);
    ASSERT(!changed, "lower incarnation should be ignored");

    auto info = ml.GetMember("node-1");
    ASSERT(info->state == ALIVE, "state should still be ALIVE");
    ASSERT(info->incarnation == 10, "incarnation should still be 10");
}

// SWIM Rule: at equal incarnation, stronger state wins.
// State ordering: ALIVE(0) < SUSPECT(1) < DEAD(2).
// This means SUSPECT overrides ALIVE at the same incarnation,
// but ALIVE does NOT override SUSPECT at the same incarnation.
void test_equal_incarnation_stronger_state_wins() {
    MembershipList ml("self");

    MembershipUpdate u1;
    u1.set_member_id("node-1");
    u1.set_address("127.0.0.1:7001");
    u1.set_state(ALIVE);
    u1.set_incarnation(3);
    ml.ApplyUpdate(u1);

    // SUSPECT at same incarnation should override ALIVE.
    MembershipUpdate u2;
    u2.set_member_id("node-1");
    u2.set_address("127.0.0.1:7001");
    u2.set_state(SUSPECT);
    u2.set_incarnation(3);
    bool changed = ml.ApplyUpdate(u2);
    ASSERT(changed, "SUSPECT at same incarnation should override ALIVE");
    ASSERT(ml.GetMember("node-1")->state == SUSPECT, "should be SUSPECT");

    // ALIVE at same incarnation should NOT override SUSPECT.
    MembershipUpdate u3;
    u3.set_member_id("node-1");
    u3.set_address("127.0.0.1:7001");
    u3.set_state(ALIVE);
    u3.set_incarnation(3);
    changed = ml.ApplyUpdate(u3);
    ASSERT(!changed, "ALIVE at same incarnation should not override SUSPECT");
    ASSERT(ml.GetMember("node-1")->state == SUSPECT, "should still be SUSPECT");
}

// SWIM Refutation: a suspected node can clear the suspicion by broadcasting
// an ALIVE with a higher incarnation number. This is the key mechanism that
// prevents false positives — a slow but alive node increments its incarnation
// to prove it's still running.
void test_refutation_higher_incarnation() {
    MembershipList ml("self");

    // node-1 is SUSPECT at incarnation 3.
    MembershipUpdate u1;
    u1.set_member_id("node-1");
    u1.set_address("127.0.0.1:7001");
    u1.set_state(SUSPECT);
    u1.set_incarnation(3);
    ml.ApplyUpdate(u1);

    // node-1 refutes with ALIVE at incarnation 4.
    MembershipUpdate u2;
    u2.set_member_id("node-1");
    u2.set_address("127.0.0.1:7001");
    u2.set_state(ALIVE);
    u2.set_incarnation(4);
    bool changed = ml.ApplyUpdate(u2);
    ASSERT(changed, "refutation (higher incarnation ALIVE) should override SUSPECT");
    ASSERT(ml.GetMember("node-1")->state == ALIVE, "should be ALIVE after refutation");
    ASSERT(ml.GetMember("node-1")->incarnation == 4, "incarnation should be 4");
}

// SWIM Rule: DEAD overrides everything regardless of incarnation.
// Once a node is declared dead (suspect timer expired), only a rejoin
// with a new incarnation can bring it back.
void test_dead_overrides_everything() {
    MembershipList ml("self");

    MembershipUpdate u1;
    u1.set_member_id("node-1");
    u1.set_address("127.0.0.1:7001");
    u1.set_state(ALIVE);
    u1.set_incarnation(100);
    ml.ApplyUpdate(u1);

    // DEAD with lower incarnation should still override.
    MembershipUpdate u2;
    u2.set_member_id("node-1");
    u2.set_address("127.0.0.1:7001");
    u2.set_state(DEAD);
    u2.set_incarnation(1);
    bool changed = ml.ApplyUpdate(u2);
    ASSERT(changed, "DEAD should override ALIVE regardless of incarnation");
    ASSERT(ml.GetMember("node-1")->state == DEAD, "should be DEAD");
}

// Verify the explicit MarkSuspect/MarkDead methods used by the SWIM sender
// thread when ping timeouts occur and suspect timers expire.
void test_mark_suspect_and_dead() {
    MembershipList ml("self");
    ml.AddMember("node-1", "127.0.0.1:7001");

    bool ok = ml.MarkSuspect("node-1");
    ASSERT(ok, "MarkSuspect should succeed");
    ASSERT(ml.GetMember("node-1")->state == SUSPECT, "should be SUSPECT");

    ok = ml.MarkDead("node-1");
    ASSERT(ok, "MarkDead should succeed");
    ASSERT(ml.GetMember("node-1")->state == DEAD, "should be DEAD");

    // Dead node should not appear in alive list (gateway won't route to it).
    ASSERT(ml.GetAliveMembers().empty(), "no alive members expected");
}

// Self-incarnation increment is used for refutation: when a node learns
// it has been suspected, it increments its incarnation to prove it's alive.
void test_self_incarnation_increment() {
    MembershipList ml("self");
    ml.AddMember("self", "127.0.0.1:7000");

    ASSERT(ml.my_incarnation() == 0, "initial incarnation should be 0");
    ml.IncrementMyIncarnation();
    ASSERT(ml.my_incarnation() == 1, "incarnation should be 1 after increment");
    ml.IncrementMyIncarnation();
    ASSERT(ml.my_incarnation() == 2, "incarnation should be 2");
}

// Piggyback buffer: membership updates are attached to outgoing gossip messages
// for epidemic dissemination. Each update is sent multiple times (up to
// kMaxPiggybackSends) before being evicted from the buffer.
void test_piggyback_buffer() {
    MembershipList ml("self");
    ml.AddMember("node-1", "127.0.0.1:7001");
    ml.AddMember("node-2", "127.0.0.1:7002");

    // Adding 2 members should produce 2 piggybacked updates.
    auto updates = ml.GetUpdatesForPiggyback(10);
    ASSERT(updates.size() == 2, "expected 2 piggybacked updates");

    // After enough sends (> kMaxPiggybackSends), updates should be evicted.
    for (int i = 0; i < 15; i++) {
        ml.GetUpdatesForPiggyback(10);
    }
    updates = ml.GetUpdatesForPiggyback(10);
    ASSERT(updates.empty(), "piggyback buffer should be empty after enough sends");
}

// Membership callback: the gateway subscribes to membership changes to update
// its routing table. Verify the callback fires with correct old/new states.
void test_callback_on_state_change() {
    MembershipList ml("self");

    std::string last_id;
    MemberState last_old = ALIVE;
    MemberState last_new = ALIVE;
    int callback_count = 0;

    ml.SetCallback([&](const std::string& id, MemberState old_s, MemberState new_s) {
        last_id = id;
        last_old = old_s;
        last_new = new_s;
        callback_count++;
    });

    ml.AddMember("node-1", "127.0.0.1:7001");
    // AddMember doesn't fire callback (it's a new member, not a state change).

    ml.MarkSuspect("node-1");
    ASSERT(callback_count == 1, "callback should fire on MarkSuspect");
    ASSERT(last_id == "node-1", "callback should report node-1");
    ASSERT(last_old == ALIVE, "old state should be ALIVE");
    ASSERT(last_new == SUSPECT, "new state should be SUSPECT");
}

// ============================================================================
// UdpTransport tests
//
// UdpTransport handles sending/receiving Protobuf-serialized GossipMessages
// over UDP datagrams. These tests verify the serialization roundtrip and
// timeout behavior.
// ============================================================================

// Send a PING message with a piggybacked update from one transport to another.
// Verify all fields survive the serialize → UDP → deserialize roundtrip.
void test_udp_send_receive() {
    UdpTransport sender(0);       // ephemeral port (OS-assigned)
    UdpTransport receiver(17171); // fixed port

    GossipMessage msg;
    msg.set_type(PING);
    msg.set_sender_id("sender-1");
    msg.set_sequence_num(42);

    // Attach a piggybacked membership update.
    auto* update = msg.add_updates();
    update->set_member_id("node-1");
    update->set_state(ALIVE);
    update->set_incarnation(1);

    Address dest{"127.0.0.1", 17171};
    bool ok = sender.Send(dest, msg);
    ASSERT(ok, "send should succeed");

    auto result = receiver.Receive(1000);  // 1 second timeout
    ASSERT(result.has_value(), "should receive a message");

    auto& [received_msg, sender_addr] = *result;
    ASSERT(received_msg.type() == PING, "type should be PING");
    ASSERT(received_msg.sender_id() == "sender-1", "sender_id mismatch");
    ASSERT(received_msg.sequence_num() == 42, "sequence_num mismatch");
    ASSERT(received_msg.updates_size() == 1, "should have 1 piggybacked update");
    ASSERT(received_msg.updates(0).member_id() == "node-1", "update member_id mismatch");
}

// Receive with no pending messages should return nullopt after the timeout.
void test_udp_receive_timeout() {
    UdpTransport receiver(17172);

    auto result = receiver.Receive(100);  // 100ms timeout
    ASSERT(!result.has_value(), "should timeout with no message");
}

// ParseAddress should correctly split "host:port" strings.
void test_address_parse() {
    auto addr = ParseAddress("127.0.0.1:8080");
    ASSERT(addr.host == "127.0.0.1", "host mismatch");
    ASSERT(addr.port == 8080, "port mismatch");

    addr = ParseAddress("localhost:50051");
    ASSERT(addr.host == "localhost", "host mismatch");
    ASSERT(addr.port == 50051, "port mismatch");
}

// ============================================================================
// Main
// ============================================================================

int main() {
    std::cout << "=== MembershipList tests ===" << std::endl;
    TEST(add_and_get_member);
    TEST(get_alive_excludes_self);
    TEST(incarnation_higher_wins);
    TEST(incarnation_lower_ignored);
    TEST(equal_incarnation_stronger_state_wins);
    TEST(refutation_higher_incarnation);
    TEST(dead_overrides_everything);
    TEST(mark_suspect_and_dead);
    TEST(self_incarnation_increment);
    TEST(piggyback_buffer);
    TEST(callback_on_state_change);

    std::cout << std::endl << "=== UdpTransport tests ===" << std::endl;
    TEST(udp_send_receive);
    TEST(udp_receive_timeout);
    TEST(address_parse);

    std::cout << std::endl << "=== Results ===" << std::endl;
    std::cout << tests_passed << " passed, " << tests_failed << " failed" << std::endl;

    return tests_failed > 0 ? 1 : 0;
}
