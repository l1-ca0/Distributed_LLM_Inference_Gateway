#pragma once

// SWIM gossip protocol: decentralized membership and failure detection.
// Every replica and the gateway runs one SwimProtocol instance. Together
// they converge on a consistent view of alive/suspect/dead members
// without any centralized health monitor.

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "gossip/membership_list.h"
#include "gossip/udp_transport.h"

namespace llmgateway::gossip {

// Tuning parameters for the SWIM failure detector. Defaults are chosen so
// detection latency (~2 * T_suspect) stays well under typical gossip test
// budgets while keeping UDP traffic low.
struct SwimConfig {
    int protocol_period_ms = 500;   // T_protocol: time between ping rounds
    int ping_timeout_ms = 200;      // T_ping: timeout for direct/indirect ping
    int suspect_timeout_ms = 2000;  // T_suspect: time before suspect → dead
    int indirect_ping_count = 2;    // k: number of indirect probes
    int max_piggyback_updates = 8;  // max updates piggybacked per message
};

// ---------------------------------------------------------------------------
// SwimProtocol: SWIM-style gossip-based failure detector and membership
// protocol (Das, Gupta, Motivala; "SWIM", DSN 2002).
//
// Each node runs two threads:
//   - Sender loop: every T_protocol, pings a random peer, and if the direct
//     ping times out, asks k=indirect_ping_count other peers to relay a
//     ping. This indirect probing separates real failures from transient
//     network drops. Also scans for SUSPECT members whose timers have
//     expired and marks them DEAD.
//   - Receiver loop: handles inbound PING, PING_REQ, and ACK messages from
//     peers, and also processes piggybacked membership updates that ride
//     on every outgoing packet.
//
// State propagation is fully decentralized. When a node learns of a state
// change (locally observed or received from a peer), the update is queued
// in the MembershipList's piggyback buffer and attached to subsequent
// outgoing messages until it has been sent kMaxPiggybackSends times or the
// buffer fills. No heartbeats, no central health monitor.
//
// Self-refutation: if a node sees a piggybacked update marking itself
// SUSPECT/DEAD, it increments its own incarnation number and broadcasts
// an ALIVE update at the new incarnation. Because the conflict-resolution
// rules in MembershipList prefer the higher incarnation, the stale rumor
// is quickly overridden.
//
// Thread-safety:
//   - Start()/Stop() are not thread-safe; call once.
//   - SetMembershipCallback, SetMyLoad, membership_list() accessors, and
//     is_running() are safe to call at any time from any thread.
//   - The sender and receiver threads synchronize only via the
//     MembershipList (which has its own shared_mutex) and a small ACK
//     tracking table guarded by ack_mutex_.
// ---------------------------------------------------------------------------
class SwimProtocol {
public:
    SwimProtocol(const std::string& my_id, const std::string& my_address,
                 uint16_t udp_port, SwimConfig config = {});
    ~SwimProtocol();

    // Non-copyable
    SwimProtocol(const SwimProtocol&) = delete;
    SwimProtocol& operator=(const SwimProtocol&) = delete;

    // Start the protocol (launches sender + receiver threads).
    void Start();

    // Stop the protocol (signals threads and joins).
    void Stop();

    // Graceful leave: broadcast a DEAD message about ourselves to all peers
    // so they can remove us instantly (no need to wait for suspect timeout).
    // Call this before Stop() during rolling updates for faster transitions.
    void LeaveCluster();

    // Bootstrap by adding seed nodes to the membership list.
    void JoinCluster(const std::vector<std::string>& seed_addresses);

    // Set callback for membership changes (thread-safe, forwarded to MembershipList).
    void SetMembershipCallback(MembershipCallback callback);

    // Access the membership list (for gateway to query alive replicas).
    MembershipList& membership_list() { return membership_list_; }
    const MembershipList& membership_list() const { return membership_list_; }

    // Update my own load metadata (piggybacked on gossip messages).
    void SetMyLoad(int32_t active_requests, int32_t max_capacity,
                   const std::string& model_version);

    bool is_running() const { return running_.load(); }

private:
    // Thread entry points.
    // SenderLoop wakes every protocol_period_ms, runs DoPingRound, and
    // checks suspect timers. ReceiverLoop blocks on UDP recv and
    // dispatches inbound messages to the Handle* methods.
    void SenderLoop();
    void ReceiverLoop();

    // Run one SWIM probe cycle: pick a random alive peer, send PING, wait
    // for ACK up to ping_timeout_ms. On timeout, fan out PING_REQ to k
    // other peers; if none of them can reach the target either, mark the
    // target SUSPECT.
    void DoPingRound();

    // Top-level inbound dispatch. Processes any piggybacked membership
    // updates first (so Handle* methods see the latest state), then
    // routes to the per-type handler.
    void HandleMessage(const GossipMessage& msg, const Address& sender_addr);

    // Reply to a direct PING with an ACK (same seq_num), carrying our own
    // piggyback updates.
    void HandlePing(const GossipMessage& msg, const Address& sender_addr);

    // Received a PING_REQ to indirectly probe target_id: forward a PING
    // to the target and, on ACK, relay an ACK back to the original
    // requester.
    void HandlePingReq(const GossipMessage& msg, const Address& sender_addr);

    // Match the ACK's seq_num against pending_acks_ and signal the
    // waiting thread (which is blocked in WaitForAck).
    void HandleAck(const GossipMessage& msg);

    // Apply every MembershipUpdate piggybacked on a message to the local
    // membership list, fire the self-refutation check for each, and let
    // MembershipList's conflict-resolution rules decide which updates
    // actually take effect.
    void ProcessPiggybackedUpdates(const GossipMessage& msg);

    // Promote members whose SUSPECT timer has exceeded suspect_timeout_ms
    // to DEAD. Called once per protocol period by the sender loop.
    void CheckSuspectTimeouts();

    // Build a GossipMessage of the given type, attach up to
    // max_piggyback_updates pending updates, and send it to dest. Returns
    // after the UDP send completes; acks (if any) are awaited separately.
    void SendMessage(const Address& dest, MessageType type,
                     const std::string& target_id, uint64_t seq_num);

    // Block up to timeout_ms for an ACK with the given seq_num to arrive
    // (via the receiver thread calling RecordAck). Returns true if the
    // ACK was received, false on timeout.
    bool WaitForAck(uint64_t seq_num, int timeout_ms);

    // Mark the given seq_num as received and wake any thread waiting in
    // WaitForAck.
    void RecordAck(uint64_t seq_num);

    // If an incoming update declares us SUSPECT or DEAD at our current
    // incarnation, bump our incarnation and broadcast ALIVE so peers
    // override the stale rumor.
    void CheckSelfRefutation(const MembershipUpdate& update);

    std::string my_id_;
    std::string my_address_;
    SwimConfig config_;

    UdpTransport transport_;
    MembershipList membership_list_;

    std::atomic<bool> running_{false};
    std::thread sender_thread_;
    std::thread receiver_thread_;

    // Pending ACKs: sender waits, receiver notifies.
    std::mutex ack_mutex_;
    std::condition_variable ack_cv_;
    std::unordered_map<uint64_t, bool> pending_acks_;  // seq_num → received

    // Sequence number counter (atomic, monotonically increasing).
    std::atomic<uint64_t> next_seq_num_{1};

    // My load metadata (updated by the replica/gateway, piggybacked on messages).
    std::mutex load_mutex_;
    int32_t my_active_requests_ = 0;
    int32_t my_max_capacity_ = 0;
    std::string my_model_version_;
};

}  // namespace llmgateway::gossip
