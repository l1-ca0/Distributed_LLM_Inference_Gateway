#pragma once

#include <chrono>
#include <deque>
#include <functional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "gossip.pb.h"

namespace llmgateway::gossip {

struct MemberInfo {
    std::string address;       // host:port (gossip port)
    MemberState state = ALIVE;
    uint64_t incarnation = 0;
    std::string model_version;
    int32_t active_requests = 0;
    int32_t max_capacity = 0;
    std::chrono::steady_clock::time_point suspect_time;  // when state became SUSPECT
};

// Callback invoked when membership changes.
// Parameters: member_id, old_state, new_state
using MembershipCallback =
    std::function<void(const std::string& member_id, MemberState old_state,
                       MemberState new_state)>;

// Thread-safe membership list with SWIM incarnation-based conflict resolution.
//
// Incarnation resolution rules:
//   - Higher incarnation always wins for the same member.
//   - At equal incarnation: DEAD > SUSPECT > ALIVE (stronger state wins).
//   - A member can refute a SUSPECT about itself by incrementing its incarnation.
class MembershipList {
public:
    explicit MembershipList(const std::string& my_id);

    // Apply a membership update. Returns true if the update changed state.
    // This is the core conflict resolution logic.
    bool ApplyUpdate(const MembershipUpdate& update);

    // Mark a member as SUSPECT (called by SWIM sender on ping timeout).
    // Uses the member's current incarnation — does NOT increment it.
    bool MarkSuspect(const std::string& member_id);

    // Mark a member as DEAD (called when suspect timer expires).
    bool MarkDead(const std::string& member_id);

    // Add a known member (e.g., seed node during bootstrap).
    void AddMember(const std::string& member_id, const std::string& address);

    // Get info for a specific member. Returns nullptr if not found.
    // The returned pointer is valid only while the caller holds no lock
    // (copy the data if needed across lock boundaries).
    std::optional<MemberInfo> GetMember(const std::string& member_id) const;

    // Get all members in a given state.
    std::vector<std::pair<std::string, MemberInfo>> GetMembersByState(
        MemberState state) const;

    // Get all alive members (convenience).
    std::vector<std::pair<std::string, MemberInfo>> GetAliveMembers() const {
        return GetMembersByState(ALIVE);
    }

    // Get all alive + suspect members (for routing with deprioritization).
    std::vector<std::pair<std::string, MemberInfo>> GetRoutableMembers() const;

    // Get a random alive member (excluding self and optionally excluding others).
    std::optional<std::pair<std::string, MemberInfo>> GetRandomAlivePeer(
        const std::vector<std::string>& exclude = {}) const;

    // Get up to max_count recent updates for piggybacking on outgoing messages.
    // Updates are returned in priority order (newer first).
    // Each call increments the send count; updates are removed after enough sends.
    std::vector<MembershipUpdate> GetUpdatesForPiggyback(int max_count);

    // Get my own incarnation number.
    uint64_t my_incarnation() const;

    // Increment my incarnation (for self-refutation) and queue an ALIVE update.
    void IncrementMyIncarnation();

    // Set callback for membership changes.
    void SetCallback(MembershipCallback callback);

    // Total member count (all states).
    size_t Size() const;

    // Get my ID.
    const std::string& my_id() const { return my_id_; }

private:
    // Queue an update for piggybacking.
    void QueueUpdate(const std::string& member_id, const MemberInfo& info);

    std::string my_id_;
    uint64_t my_incarnation_ = 0;

    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, MemberInfo> members_;

    // Piggyback buffer: recent updates to disseminate.
    struct PiggybackEntry {
        MembershipUpdate update;
        int send_count = 0;
    };
    std::deque<PiggybackEntry> piggyback_buffer_;
    static constexpr int kMaxPiggybackSends = 10;  // remove after this many sends
    static constexpr int kMaxPiggybackBuffer = 50;  // max buffer size

    MembershipCallback callback_;
};

}  // namespace llmgateway::gossip
