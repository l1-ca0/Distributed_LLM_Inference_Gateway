#include "gossip/membership_list.h"

#include <algorithm>
#include <random>

namespace llmgateway::gossip {

// Thread-local random engine for peer selection.
static thread_local std::mt19937 g_rng{std::random_device{}()};

MembershipList::MembershipList(const std::string& my_id) : my_id_(my_id) {}

bool MembershipList::ApplyUpdate(const MembershipUpdate& update) {
    std::unique_lock lock(mutex_);

    const auto& id = update.member_id();
    auto it = members_.find(id);

    MemberState old_state = DEAD;  // treat unknown as dead for callback purposes
    int32_t old_max_capacity = 0;
    int32_t old_active_requests = 0;
    std::string old_model_version;

    if (it != members_.end()) {
        old_state = it->second.state;
        old_max_capacity = it->second.max_capacity;
        old_active_requests = it->second.active_requests;
        old_model_version = it->second.model_version;
        auto& existing = it->second;

        // ---- Incarnation-based conflict resolution ----
        // Rule 1: Higher incarnation always wins.
        // Rule 2: At equal incarnation, stronger state wins (DEAD > SUSPECT > ALIVE).
        // Rule 3: DEAD overrides everything regardless of incarnation.

        if (update.state() == DEAD) {
            // DEAD always wins — a dead declaration is final until rejoin.
            // (Rejoin happens with a new, higher incarnation.)
        } else if (update.incarnation() < existing.incarnation) {
            // Stale update — ignore.
            return false;
        } else if (update.incarnation() == existing.incarnation) {
            // Same incarnation: stronger state wins.
            // State ordering: ALIVE(0) < SUSPECT(1) < DEAD(2)
            if (update.state() <= existing.state) {
                // Same or weaker state — update load info only.
                bool load_changed = false;
                if (update.active_requests() != 0 || update.max_capacity() != 0) {
                    if (existing.active_requests != update.active_requests() ||
                        existing.max_capacity != update.max_capacity()) {
                        load_changed = true;
                    }
                    existing.active_requests = update.active_requests();
                    existing.max_capacity = update.max_capacity();
                }
                if (!update.model_version().empty() &&
                    existing.model_version != update.model_version()) {
                    existing.model_version = update.model_version();
                    load_changed = true;
                }
                // Fire callback on meaningful metadata change (same state) so
                // downstream subscribers (e.g., the gateway's ReplicaRegistry)
                // can refresh capacity/version info that they cached at the
                // initial state transition — which often carries zero-valued
                // placeholder metadata from bootstrap AddMember calls.
                if (load_changed && callback_) {
                    auto cb = callback_;
                    MemberState s = existing.state;
                    lock.unlock();
                    cb(id, s, s);
                }
                return false;  // No state change.
            }
            // Stronger state at same incarnation — fall through to apply.
        }
        // Higher incarnation or stronger state — fall through to apply.
    }

    // Apply the update.
    MemberInfo& info = members_[id];
    MemberState new_state = update.state();

    info.address = update.address();
    info.state = new_state;
    info.incarnation = update.incarnation();
    if (!update.model_version().empty()) {
        info.model_version = update.model_version();
    }
    if (update.active_requests() != 0 || update.max_capacity() != 0) {
        info.active_requests = update.active_requests();
        info.max_capacity = update.max_capacity();
    }

    if (new_state == SUSPECT) {
        info.suspect_time = std::chrono::steady_clock::now();
    }

    // Queue for piggybacking.
    QueueUpdate(id, info);

    // Fire callback if state actually changed.
    if (new_state != old_state && callback_) {
        // Release lock before callback to avoid deadlocks.
        auto cb = callback_;
        lock.unlock();
        cb(id, old_state, new_state);
    }

    return true;
}

bool MembershipList::MarkSuspect(const std::string& member_id) {
    std::unique_lock lock(mutex_);
    auto it = members_.find(member_id);
    if (it == members_.end() || it->second.state != ALIVE) {
        return false;
    }

    MemberState old_state = it->second.state;
    it->second.state = SUSPECT;
    it->second.suspect_time = std::chrono::steady_clock::now();

    QueueUpdate(member_id, it->second);

    if (callback_) {
        auto cb = callback_;
        lock.unlock();
        cb(member_id, old_state, SUSPECT);
    }

    return true;
}

bool MembershipList::MarkDead(const std::string& member_id) {
    std::unique_lock lock(mutex_);
    auto it = members_.find(member_id);
    if (it == members_.end() || it->second.state == DEAD) {
        return false;
    }

    MemberState old_state = it->second.state;
    it->second.state = DEAD;

    QueueUpdate(member_id, it->second);

    if (callback_) {
        auto cb = callback_;
        lock.unlock();
        cb(member_id, old_state, DEAD);
    }

    return true;
}

void MembershipList::AddMember(const std::string& member_id,
                               const std::string& address) {
    std::unique_lock lock(mutex_);
    if (members_.find(member_id) != members_.end()) {
        return;  // Already known.
    }
    MemberInfo info;
    info.address = address;
    info.state = ALIVE;
    info.incarnation = 0;
    members_[member_id] = info;

    QueueUpdate(member_id, info);
}

std::optional<MemberInfo> MembershipList::GetMember(
    const std::string& member_id) const {
    std::shared_lock lock(mutex_);
    auto it = members_.find(member_id);
    if (it == members_.end()) return std::nullopt;
    return it->second;
}

std::vector<std::pair<std::string, MemberInfo>> MembershipList::GetMembersByState(
    MemberState state) const {
    std::shared_lock lock(mutex_);
    std::vector<std::pair<std::string, MemberInfo>> result;
    for (const auto& [id, info] : members_) {
        if (id != my_id_ && info.state == state) {
            result.emplace_back(id, info);
        }
    }
    return result;
}

std::vector<std::pair<std::string, MemberInfo>>
MembershipList::GetRoutableMembers() const {
    std::shared_lock lock(mutex_);
    std::vector<std::pair<std::string, MemberInfo>> result;
    for (const auto& [id, info] : members_) {
        if (id != my_id_ && (info.state == ALIVE || info.state == SUSPECT)) {
            result.emplace_back(id, info);
        }
    }
    return result;
}

std::optional<std::pair<std::string, MemberInfo>>
MembershipList::GetRandomAlivePeer(
    const std::vector<std::string>& exclude) const {
    std::shared_lock lock(mutex_);

    std::vector<std::pair<std::string, MemberInfo>> candidates;
    for (const auto& [id, info] : members_) {
        if (id == my_id_) continue;
        if (info.state != ALIVE && info.state != SUSPECT) continue;
        if (std::find(exclude.begin(), exclude.end(), id) != exclude.end()) continue;
        candidates.emplace_back(id, info);
    }

    if (candidates.empty()) return std::nullopt;

    std::uniform_int_distribution<size_t> dist(0, candidates.size() - 1);
    return candidates[dist(g_rng)];
}

std::vector<MembershipUpdate> MembershipList::GetUpdatesForPiggyback(
    int max_count) {
    std::unique_lock lock(mutex_);

    std::vector<MembershipUpdate> result;
    result.reserve(std::min(max_count, static_cast<int>(piggyback_buffer_.size())));

    for (auto& entry : piggyback_buffer_) {
        if (static_cast<int>(result.size()) >= max_count) break;
        result.push_back(entry.update);
        entry.send_count++;
    }

    // Remove entries that have been sent enough times.
    while (!piggyback_buffer_.empty() &&
           piggyback_buffer_.front().send_count >= kMaxPiggybackSends) {
        piggyback_buffer_.pop_front();
    }

    return result;
}

uint64_t MembershipList::my_incarnation() const {
    std::shared_lock lock(mutex_);
    return my_incarnation_;
}

void MembershipList::IncrementMyIncarnation() {
    std::unique_lock lock(mutex_);
    my_incarnation_++;

    // Queue an ALIVE update about ourselves for piggybacking.
    MemberInfo self_info;
    auto it = members_.find(my_id_);
    if (it != members_.end()) {
        self_info = it->second;
    }
    self_info.state = ALIVE;
    self_info.incarnation = my_incarnation_;

    // Update our own entry.
    members_[my_id_] = self_info;

    QueueUpdate(my_id_, self_info);
}

void MembershipList::SetCallback(MembershipCallback callback) {
    std::unique_lock lock(mutex_);
    callback_ = std::move(callback);
}

size_t MembershipList::Size() const {
    std::shared_lock lock(mutex_);
    return members_.size();
}

void MembershipList::QueueUpdate(const std::string& member_id,
                                 const MemberInfo& info) {
    // Caller must hold unique_lock on mutex_.
    MembershipUpdate update;
    update.set_member_id(member_id);
    update.set_address(info.address);
    update.set_state(info.state);
    update.set_incarnation(info.incarnation);
    update.set_model_version(info.model_version);
    update.set_active_requests(info.active_requests);
    update.set_max_capacity(info.max_capacity);

    // Trim buffer if too large.
    if (piggyback_buffer_.size() >= kMaxPiggybackBuffer) {
        piggyback_buffer_.pop_front();
    }

    piggyback_buffer_.push_back({std::move(update), 0});
}

}  // namespace llmgateway::gossip
