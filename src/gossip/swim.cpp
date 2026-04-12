#include "gossip/swim.h"

#include <algorithm>
#include <chrono>
#include <random>

#include "common/log.h"

namespace llmgateway::gossip {

static thread_local std::mt19937 g_rng{std::random_device{}()};

SwimProtocol::SwimProtocol(const std::string& my_id,
                           const std::string& my_address, uint16_t udp_port,
                           SwimConfig config)
    : my_id_(my_id),
      my_address_(my_address),
      config_(config),
      transport_(udp_port),
      membership_list_(my_id) {
    // Add ourselves to the membership list.
    membership_list_.AddMember(my_id, my_address);
}

SwimProtocol::~SwimProtocol() {
    Stop();
}

void SwimProtocol::Start() {
    if (running_.load()) return;
    running_.store(true);

    receiver_thread_ = std::thread(&SwimProtocol::ReceiverLoop, this);
    sender_thread_ = std::thread(&SwimProtocol::SenderLoop, this);

    LOG_INFO("swim", "%s started on port %d", my_id_.c_str(), transport_.port());
}

void SwimProtocol::Stop() {
    if (!running_.load()) return;
    running_.store(false);

    if (sender_thread_.joinable()) sender_thread_.join();
    if (receiver_thread_.joinable()) receiver_thread_.join();

    LOG_INFO("swim", "%s stopped", my_id_.c_str());
}

void SwimProtocol::LeaveCluster() {
    // Broadcast a DEAD message about ourselves to all alive peers.
    // This allows instant removal without waiting for the suspect timeout.
    auto peers = membership_list_.GetAliveMembers();

    // Create a DEAD update about ourselves with a high incarnation.
    MembershipUpdate leave_update;
    leave_update.set_member_id(my_id_);
    leave_update.set_address(my_address_);
    leave_update.set_state(DEAD);
    leave_update.set_incarnation(membership_list_.my_incarnation() + 1);

    for (const auto& [peer_id, peer_info] : peers) {
        GossipMessage msg;
        msg.set_type(PING);  // Use PING so the peer processes piggybacked updates
        msg.set_sender_id(my_id_);
        msg.set_sequence_num(next_seq_num_.fetch_add(1));
        *msg.add_updates() = leave_update;

        Address dest = ParseAddress(peer_info.address);
        transport_.Send(dest, msg);
    }

    LOG_INFO("swim", "%s sent graceful leave to %zu peers", my_id_.c_str(), peers.size());
}

void SwimProtocol::JoinCluster(const std::vector<std::string>& seed_addresses) {
    for (const auto& addr : seed_addresses) {
        // Don't add ourselves as a seed.
        if (addr == my_address_) continue;

        // Derive a temporary ID from the address until we learn the real ID via gossip.
        // The real ID will be learned when the seed responds with piggybacked updates.
        std::string seed_id = "seed-" + addr;
        membership_list_.AddMember(seed_id, addr);
    }
}

void SwimProtocol::SetMembershipCallback(MembershipCallback callback) {
    membership_list_.SetCallback(std::move(callback));
}

void SwimProtocol::SetMyLoad(int32_t active_requests, int32_t max_capacity,
                             const std::string& model_version) {
    std::lock_guard lock(load_mutex_);
    my_active_requests_ = active_requests;
    my_max_capacity_ = max_capacity;
    my_model_version_ = model_version;
}

// ============================================================================
// Sender thread: periodic PING rounds
// ============================================================================

void SwimProtocol::SenderLoop() {
    while (running_.load()) {
        auto round_start = std::chrono::steady_clock::now();

        DoPingRound();
        CheckSuspectTimeouts();

        // Sleep for the remainder of the protocol period.
        auto elapsed = std::chrono::steady_clock::now() - round_start;
        auto sleep_time = std::chrono::milliseconds(config_.protocol_period_ms) - elapsed;
        if (sleep_time > std::chrono::milliseconds(0)) {
            std::this_thread::sleep_for(sleep_time);
        }
    }
}

void SwimProtocol::DoPingRound() {
    // Pick a random alive peer.
    auto peer = membership_list_.GetRandomAlivePeer();
    if (!peer) return;  // No peers to ping.

    const auto& [peer_id, peer_info] = *peer;
    uint64_t seq = next_seq_num_.fetch_add(1);

    // Register pending ACK before sending.
    {
        std::lock_guard lock(ack_mutex_);
        pending_acks_[seq] = false;
    }

    // Send PING.
    Address dest = ParseAddress(peer_info.address);
    SendMessage(dest, PING, "", seq);

    // Wait for ACK.
    if (WaitForAck(seq, config_.ping_timeout_ms)) {
        // Direct ping succeeded — peer is alive.
        // Clean up pending ACK.
        std::lock_guard lock(ack_mutex_);
        pending_acks_.erase(seq);
        return;
    }

    // Direct ping failed — try indirect probes.
    // Pick k random other alive peers (excluding the suspect and ourselves).
    std::vector<std::string> exclude = {peer_id};
    std::vector<uint64_t> indirect_seqs;

    for (int i = 0; i < config_.indirect_ping_count; i++) {
        auto proxy = membership_list_.GetRandomAlivePeer(exclude);
        if (!proxy) break;

        uint64_t indirect_seq = next_seq_num_.fetch_add(1);
        {
            std::lock_guard lock(ack_mutex_);
            pending_acks_[indirect_seq] = false;
        }

        Address proxy_addr = ParseAddress(proxy->second.address);
        SendMessage(proxy_addr, PING_REQ, peer_id, indirect_seq);
        indirect_seqs.push_back(indirect_seq);

        exclude.push_back(proxy->first);
    }

    // Wait for any indirect ACK.
    bool got_indirect_ack = false;
    if (!indirect_seqs.empty()) {
        // Wait for the ping timeout — check if any indirect ACK arrived.
        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(config_.ping_timeout_ms);

        std::unique_lock lock(ack_mutex_);
        ack_cv_.wait_until(lock, deadline, [&]() {
            for (uint64_t s : indirect_seqs) {
                if (pending_acks_.count(s) && pending_acks_[s]) return true;
            }
            return false;
        });

        for (uint64_t s : indirect_seqs) {
            if (pending_acks_.count(s) && pending_acks_[s]) {
                got_indirect_ack = true;
            }
            pending_acks_.erase(s);
        }
    }

    // Clean up the original direct ping ACK entry.
    {
        std::lock_guard lock(ack_mutex_);
        pending_acks_.erase(seq);
    }

    if (got_indirect_ack) {
        // Indirect probe succeeded — peer is alive.
        return;
    }

    // Both direct and indirect probes failed — mark suspect.
    membership_list_.MarkSuspect(peer_id);
}

void SwimProtocol::CheckSuspectTimeouts() {
    auto suspects = membership_list_.GetMembersByState(SUSPECT);
    auto now = std::chrono::steady_clock::now();

    for (const auto& [id, info] : suspects) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - info.suspect_time);
        if (elapsed.count() >= config_.suspect_timeout_ms) {
            membership_list_.MarkDead(id);
        }
    }
}

// ============================================================================
// Receiver thread: listen for incoming messages
// ============================================================================

void SwimProtocol::ReceiverLoop() {
    while (running_.load()) {
        auto result = transport_.Receive(100);  // 100ms timeout
        if (!result) continue;

        auto& [msg, sender_addr] = *result;
        HandleMessage(msg, sender_addr);
    }
}

void SwimProtocol::HandleMessage(const GossipMessage& msg,
                                 const Address& sender_addr) {
    // Always process piggybacked membership updates first.
    ProcessPiggybackedUpdates(msg);

    switch (msg.type()) {
        case PING:
            HandlePing(msg, sender_addr);
            break;
        case PING_REQ:
            HandlePingReq(msg, sender_addr);
            break;
        case ACK:
            HandleAck(msg);
            break;
        default:
            break;
    }
}

void SwimProtocol::HandlePing(const GossipMessage& msg,
                              const Address& sender_addr) {
    // Learn the sender's real ID and address.
    if (!msg.sender_id().empty()) {
        membership_list_.AddMember(msg.sender_id(), sender_addr.ToString());
    }

    // Reply with ACK (same sequence number).
    SendMessage(sender_addr, ACK, "", msg.sequence_num());
}

void SwimProtocol::HandlePingReq(const GossipMessage& msg,
                                 const Address& sender_addr) {
    // We are a proxy: forward PING to the target, and if we get an ACK,
    // relay it back to the original sender.
    const auto& target_id = msg.target_id();
    auto target_info = membership_list_.GetMember(target_id);
    if (!target_info) return;  // Unknown target.

    // Send PING to the target.
    uint64_t probe_seq = next_seq_num_.fetch_add(1);
    {
        std::lock_guard lock(ack_mutex_);
        pending_acks_[probe_seq] = false;
    }

    Address target_addr = ParseAddress(target_info->address);
    SendMessage(target_addr, PING, "", probe_seq);

    // Wait for ACK from target.
    if (WaitForAck(probe_seq, config_.ping_timeout_ms)) {
        // Target is alive — relay ACK back to the original requester.
        SendMessage(sender_addr, ACK, "", msg.sequence_num());
    }

    // Clean up.
    std::lock_guard lock(ack_mutex_);
    pending_acks_.erase(probe_seq);
}

void SwimProtocol::HandleAck(const GossipMessage& msg) {
    RecordAck(msg.sequence_num());
}

void SwimProtocol::ProcessPiggybackedUpdates(const GossipMessage& msg) {
    for (const auto& update : msg.updates()) {
        // Check for self-refutation before applying.
        CheckSelfRefutation(update);

        membership_list_.ApplyUpdate(update);
    }
}

void SwimProtocol::CheckSelfRefutation(const MembershipUpdate& update) {
    if (update.member_id() != my_id_) return;

    // Someone thinks we are SUSPECT or DEAD — refute by incrementing incarnation.
    if (update.state() == SUSPECT || update.state() == DEAD) {
        if (update.incarnation() >= membership_list_.my_incarnation()) {
            membership_list_.IncrementMyIncarnation();
        }
    }
}

// ============================================================================
// Message sending with piggybacked updates
// ============================================================================

void SwimProtocol::SendMessage(const Address& dest, MessageType type,
                               const std::string& target_id, uint64_t seq_num) {
    GossipMessage msg;
    msg.set_type(type);
    msg.set_sender_id(my_id_);
    msg.set_target_id(target_id);
    msg.set_sequence_num(seq_num);

    // Piggyback recent membership updates.
    auto updates = membership_list_.GetUpdatesForPiggyback(
        config_.max_piggyback_updates);
    for (auto& update : updates) {
        *msg.add_updates() = std::move(update);
    }

    // Also piggyback our own load metadata.
    {
        std::lock_guard lock(load_mutex_);
        auto* self_update = msg.add_updates();
        self_update->set_member_id(my_id_);
        self_update->set_address(my_address_);
        self_update->set_state(ALIVE);
        self_update->set_incarnation(membership_list_.my_incarnation());
        self_update->set_active_requests(my_active_requests_);
        self_update->set_max_capacity(my_max_capacity_);
        self_update->set_model_version(my_model_version_);
    }

    transport_.Send(dest, msg);
}

// ============================================================================
// ACK waiting
// ============================================================================

bool SwimProtocol::WaitForAck(uint64_t seq_num, int timeout_ms) {
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);

    std::unique_lock lock(ack_mutex_);
    return ack_cv_.wait_until(lock, deadline, [&]() {
        return pending_acks_.count(seq_num) && pending_acks_[seq_num];
    });
}

void SwimProtocol::RecordAck(uint64_t seq_num) {
    std::lock_guard lock(ack_mutex_);
    if (pending_acks_.count(seq_num)) {
        pending_acks_[seq_num] = true;
        ack_cv_.notify_all();
    }
}

}  // namespace llmgateway::gossip
