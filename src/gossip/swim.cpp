// SWIM protocol implementation.
//
// Reference: Das et al., "SWIM: Scalable Weakly-consistent Infection-style
// Process Group Membership Protocol", DSN 2002.
//
// Architecture:
//   Two threads — a sender (periodic PING loop) and a receiver (UDP listen loop).
//   They communicate via a pending_acks map with condition_variable notification.
//   The sender waits for ACKs; the receiver deposits them.
//
// Failure detection pipeline (per protocol round):
//   1. Select random alive peer → send PING
//   2. If ACK received within T_ping → peer alive, done
//   3. If no ACK → select k random proxies → send PING_REQ (indirect probe)
//   4. If any proxy relays an ACK → peer alive, done
//   5. If no indirect ACK → mark peer SUSPECT
//   6. If peer stays SUSPECT for T_suspect → mark DEAD
//   7. If peer receives its own SUSPECT → increment incarnation → refute
//
// Dissemination:
//   Membership updates are piggybacked on every outgoing message (PING, ACK,
//   PING_REQ). This provides O(log N) convergence without dedicated gossip
//   rounds, using the protocol's existing message traffic as a carrier.

#include "gossip/swim.h"

#include <algorithm>
#include <chrono>
#include <mutex>
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
    membership_list_.AddMember(my_id, my_address);
}

SwimProtocol::~SwimProtocol() {
    Stop();
}

void SwimProtocol::Start() {
    if (running_.load()) return;
    running_.store(true);

    // Receiver must start before sender so incoming ACKs aren't missed.
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

// Graceful leave: broadcast a DEAD message about ourselves so peers can
// remove us instantly without waiting for the suspect timeout. This is an
// optimization for rolling updates — drain → graceful leave → restart is
// significantly faster than drain → crash detection → restart.
void SwimProtocol::LeaveCluster() {
    auto peers = membership_list_.GetAliveMembers();

    // Use incarnation + 1 to ensure this overrides any concurrent ALIVE
    // messages that might be in-flight from our own piggybacked updates.
    MembershipUpdate leave_update;
    leave_update.set_member_id(my_id_);
    leave_update.set_address(my_address_);
    leave_update.set_state(DEAD);
    leave_update.set_incarnation(membership_list_.my_incarnation() + 1);

    for (const auto& [peer_id, peer_info] : peers) {
        GossipMessage msg;
        msg.set_type(PING);
        msg.set_sender_id(my_id_);
        msg.set_sequence_num(next_seq_num_.fetch_add(1));
        *msg.add_updates() = leave_update;

        Address dest = ParseAddress(peer_info.address);
        transport_.Send(dest, msg);
    }

    LOG_INFO("swim", "%s sent graceful leave to %zu peers", my_id_.c_str(), peers.size());
}

// Bootstrap: add seed nodes so the first protocol round has peers to ping.
// Seeds are added with a temporary "seed-<addr>" ID. The real ID is learned
// when the seed responds and its sender_id is extracted from the PING/ACK.
void SwimProtocol::JoinCluster(const std::vector<std::string>& seed_addresses) {
    for (const auto& addr : seed_addresses) {
        if (addr == my_address_) continue;
        std::string seed_id = "seed-" + addr;
        membership_list_.AddMember(seed_id, addr);
    }
}

void SwimProtocol::SetMembershipCallback(MembershipCallback callback) {
    membership_list_.SetCallback(std::move(callback));
}

// Publish load metadata that will be piggybacked on outgoing gossip messages.
// The gateway reads this to make load-aware routing decisions.
void SwimProtocol::SetMyLoad(int32_t active_requests, int32_t max_capacity,
                             const std::string& model_version) {
    std::lock_guard lock(load_mutex_);
    my_active_requests_ = active_requests;
    my_max_capacity_ = max_capacity;
    my_model_version_ = model_version;
}

// ============================================================================
// Sender thread
//
// Runs one "protocol round" every T_protocol milliseconds. Each round:
//   1. Pings a random peer (direct probe)
//   2. If no ACK, tries indirect probes through k other peers
//   3. If still no ACK, marks the peer as SUSPECT
//   4. Scans suspect list and promotes timed-out suspects to DEAD
// ============================================================================

void SwimProtocol::SenderLoop() {
    while (running_.load()) {
        auto round_start = std::chrono::steady_clock::now();

        DoPingRound();
        CheckSuspectTimeouts();

        // Maintain consistent round timing by sleeping for the remainder.
        // If the round took longer than T_protocol (e.g., due to slow indirect
        // probes), the next round starts immediately with no sleep.
        auto elapsed = std::chrono::steady_clock::now() - round_start;
        auto sleep_time = std::chrono::milliseconds(config_.protocol_period_ms) - elapsed;
        if (sleep_time > std::chrono::milliseconds(0)) {
            std::this_thread::sleep_for(sleep_time);
        }
    }
}

void SwimProtocol::DoPingRound() {
    // Step 1: Select a random alive peer to probe.
    auto peer = membership_list_.GetRandomAlivePeer();
    if (!peer) return;

    const auto& [peer_id, peer_info] = *peer;
    uint64_t seq = next_seq_num_.fetch_add(1);

    // Register the pending ACK before sending, so the receiver thread can
    // record it even if the response arrives very quickly.
    {
        std::lock_guard lock(ack_mutex_);
        pending_acks_[seq] = false;
    }

    // Step 2: Send direct PING.
    Address dest = ParseAddress(peer_info.address);
    SendMessage(dest, PING, "", seq);

    // Step 3: Wait for direct ACK.
    if (WaitForAck(seq, config_.ping_timeout_ms)) {
        std::lock_guard lock(ack_mutex_);
        pending_acks_.erase(seq);
        return;  // Peer is alive.
    }

    // Step 4: Direct ping failed — try indirect probes.
    // Ask k random other alive peers to ping the suspect on our behalf.
    // This handles cases where the direct network path is temporarily broken
    // but the suspect is reachable through other nodes.
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

    // Step 5: Wait for any indirect ACK (any one is enough to confirm alive).
    bool got_indirect_ack = false;
    if (!indirect_seqs.empty()) {
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

    {
        std::lock_guard lock(ack_mutex_);
        pending_acks_.erase(seq);
    }

    if (got_indirect_ack) {
        return;  // Indirect probe succeeded — peer is alive.
    }

    // Step 6: Both direct and indirect probes failed — mark SUSPECT.
    // The peer gets T_suspect to refute (via incarnation increment) before
    // being declared DEAD.
    membership_list_.MarkSuspect(peer_id);
}

// Scan the suspect list and promote any member that has been SUSPECT for
// longer than T_suspect to DEAD. This runs once per protocol round.
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
// Receiver thread
//
// Listens for incoming UDP messages and dispatches to the appropriate handler.
// Runs continuously with a 100ms receive timeout to allow clean shutdown.
// ============================================================================

void SwimProtocol::ReceiverLoop() {
    while (running_.load()) {
        auto result = transport_.Receive(100);
        if (!result) continue;

        auto& [msg, sender_addr] = *result;
        HandleMessage(msg, sender_addr);
    }
}

void SwimProtocol::HandleMessage(const GossipMessage& msg,
                                 const Address& sender_addr) {
    // Always process piggybacked membership updates first, regardless of
    // message type. This is how SWIM achieves epidemic dissemination —
    // every message carries state updates as a "free" side channel.
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

// PING handler: learn the sender's identity and reply with ACK.
// The sender's real ID is extracted from the message, replacing any
// temporary "seed-<addr>" ID we may have assigned during bootstrap.
void SwimProtocol::HandlePing(const GossipMessage& msg,
                              const Address& sender_addr) {
    if (!msg.sender_id().empty()) {
        membership_list_.AddMember(msg.sender_id(), sender_addr.ToString());
    }
    SendMessage(sender_addr, ACK, "", msg.sequence_num());
}

// PING_REQ handler: we are acting as a proxy for indirect probing.
// Forward a PING to the target, and if we get an ACK, relay it back
// to the original requester. This enables failure detection even when
// the direct path between the requester and target is broken.
void SwimProtocol::HandlePingReq(const GossipMessage& msg,
                                 const Address& sender_addr) {
    const auto& target_id = msg.target_id();
    auto target_info = membership_list_.GetMember(target_id);
    if (!target_info) return;

    uint64_t probe_seq = next_seq_num_.fetch_add(1);
    {
        std::lock_guard lock(ack_mutex_);
        pending_acks_[probe_seq] = false;
    }

    Address target_addr = ParseAddress(target_info->address);
    SendMessage(target_addr, PING, "", probe_seq);

    if (WaitForAck(probe_seq, config_.ping_timeout_ms)) {
        // Target is alive — relay ACK back to the original requester
        // using their original sequence number so they can match it.
        SendMessage(sender_addr, ACK, "", msg.sequence_num());
    }

    std::lock_guard lock(ack_mutex_);
    pending_acks_.erase(probe_seq);
}

void SwimProtocol::HandleAck(const GossipMessage& msg) {
    RecordAck(msg.sequence_num());
}

// Process piggybacked membership updates from an incoming message.
// Before applying each update, check if it's a suspicion about ourselves
// that needs to be refuted.
void SwimProtocol::ProcessPiggybackedUpdates(const GossipMessage& msg) {
    for (const auto& update : msg.updates()) {
        CheckSelfRefutation(update);
        membership_list_.ApplyUpdate(update);
    }
}

// Self-refutation: if another node thinks we are SUSPECT or DEAD,
// increment our incarnation number to override the false suspicion.
// The incremented incarnation will be piggybacked on our next outgoing
// message, propagating the refutation through the cluster.
//
// This is the key mechanism that prevents false positives in SWIM:
// a slow-but-alive node can always clear its own suspicion by proving
// it's still running (via a higher incarnation number).
void SwimProtocol::CheckSelfRefutation(const MembershipUpdate& update) {
    if (update.member_id() != my_id_) return;

    if (update.state() == SUSPECT || update.state() == DEAD) {
        if (update.incarnation() >= membership_list_.my_incarnation()) {
            membership_list_.IncrementMyIncarnation();
            LOG_INFO("swim", "%s refuting %s at incarnation %lu → %lu",
                     my_id_.c_str(),
                     update.state() == SUSPECT ? "SUSPECT" : "DEAD",
                     update.incarnation(),
                     membership_list_.my_incarnation());
        }
    }
}

// ============================================================================
// Message sending
//
// Every outgoing message carries two types of piggybacked data:
//   1. Recent membership updates from the piggyback buffer (epidemic dissemination)
//   2. Our own load metadata (active_requests, max_capacity, model_version)
//
// This means every PING, ACK, and PING_REQ doubles as a gossip vector,
// spreading membership and load information without dedicated gossip rounds.
// ============================================================================

void SwimProtocol::SendMessage(const Address& dest, MessageType type,
                               const std::string& target_id, uint64_t seq_num) {
    GossipMessage msg;
    msg.set_type(type);
    msg.set_sender_id(my_id_);
    msg.set_target_id(target_id);
    msg.set_sequence_num(seq_num);

    // Attach recent membership updates for epidemic dissemination.
    auto updates = membership_list_.GetUpdatesForPiggyback(
        config_.max_piggyback_updates);
    for (auto& update : updates) {
        *msg.add_updates() = std::move(update);
    }

    // Attach our own load metadata so the gateway can make routing decisions
    // based on each replica's self-reported load.
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
// ACK coordination between sender and receiver threads
//
// The sender registers a pending ACK (seq_num → false), sends a message,
// then calls WaitForAck which blocks on a condition_variable.
// The receiver calls RecordAck when an ACK arrives, setting the flag to
// true and notifying the condition_variable.
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
