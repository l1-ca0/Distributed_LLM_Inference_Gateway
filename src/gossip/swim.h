#pragma once

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

struct SwimConfig {
    int protocol_period_ms = 500;   // T_protocol: time between ping rounds
    int ping_timeout_ms = 200;      // T_ping: timeout for direct/indirect ping
    int suspect_timeout_ms = 2000;  // T_suspect: time before suspect → dead
    int indirect_ping_count = 2;    // k: number of indirect probes
    int max_piggyback_updates = 8;  // max updates piggybacked per message
};

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
    // Thread entry points
    void SenderLoop();
    void ReceiverLoop();

    // Protocol logic
    void DoPingRound();
    void HandleMessage(const GossipMessage& msg, const Address& sender_addr);
    void HandlePing(const GossipMessage& msg, const Address& sender_addr);
    void HandlePingReq(const GossipMessage& msg, const Address& sender_addr);
    void HandleAck(const GossipMessage& msg);
    void ProcessPiggybackedUpdates(const GossipMessage& msg);
    void CheckSuspectTimeouts();

    // Send a gossip message with piggybacked membership updates.
    void SendMessage(const Address& dest, MessageType type,
                     const std::string& target_id, uint64_t seq_num);

    // Wait for an ACK with a given sequence number.
    bool WaitForAck(uint64_t seq_num, int timeout_ms);

    // Record a received ACK.
    void RecordAck(uint64_t seq_num);

    // Self-refutation: check if any piggybacked update suspects us.
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
