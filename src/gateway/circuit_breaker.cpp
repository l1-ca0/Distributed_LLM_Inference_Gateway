#include "gateway/circuit_breaker.h"

#include "common/log.h"

namespace llmgateway {

CircuitBreaker::CircuitBreaker() : config_({}) {}
CircuitBreaker::CircuitBreaker(Config config) : config_(config) {}

// ---------------------------------------------------------------------------
// AllowRequest: the gating function called before dispatching to a replica.
//
// State-by-state behavior:
//   CLOSED:    Always allow. The circuit is healthy.
//   OPEN:      Block all requests until cooldown_ms has elapsed. Once the
//              cooldown expires, transition to HALF_OPEN and allow exactly
//              one probe request through. The cooldown prevents a flood of
//              retries from hammering a degraded replica.
//   HALF_OPEN: Allow one probe at a time (probe_in_flight_ guard). If a
//              probe is already in flight, block further requests until its
//              outcome is recorded. This limits exposure: we send at most
//              one request to test whether the replica has recovered.
// ---------------------------------------------------------------------------
bool CircuitBreaker::AllowRequest() {
    std::lock_guard lock(mutex_);

    switch (state_) {
        case State::CLOSED:
            return true;

        case State::OPEN: {
            // Check if cooldown has elapsed -> transition to HALF_OPEN.
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - opened_at_);
            if (elapsed.count() >= config_.cooldown_ms) {
                state_ = State::HALF_OPEN;
                probe_in_flight_ = true;
                return true;  // Allow one probe request.
            }
            return false;  // Still in cooldown.
        }

        case State::HALF_OPEN:
            // Only allow one probe at a time.
            if (!probe_in_flight_) {
                probe_in_flight_ = true;
                return true;
            }
            return false;
    }

    return false;
}

// ---------------------------------------------------------------------------
// RecordSuccess / RecordFailure: called after a request completes.
//
// Both methods:
//   1. Append the outcome to the sliding window (deque).
//   2. Trim the window to window_size (pop_front if oversized).
//   3. Handle HALF_OPEN probe results.
//   4. Call EvaluateState() to check for CLOSED -> OPEN transition.
//
// HALF_OPEN probe logic:
//   - Success: the replica has recovered. Clear the window (fresh start)
//     and close the circuit. Clearing the window avoids carrying over old
//     failures that would immediately re-trip the circuit.
//   - Failure: the replica is still degraded. Reopen the circuit with a
//     fresh cooldown timer. Skip EvaluateState() since we already know
//     the new state (OPEN).
// ---------------------------------------------------------------------------
void CircuitBreaker::RecordSuccess() {
    std::lock_guard lock(mutex_);
    outcomes_.push_back(true);
    if (static_cast<int>(outcomes_.size()) > config_.window_size) {
        outcomes_.pop_front();
    }

    if (state_ == State::HALF_OPEN) {
        // Probe succeeded -> close the circuit, giving the replica a clean slate.
        state_ = State::CLOSED;
        probe_in_flight_ = false;
        outcomes_.clear();
    }

    EvaluateState();
}

void CircuitBreaker::RecordFailure() {
    std::lock_guard lock(mutex_);
    outcomes_.push_back(false);
    if (static_cast<int>(outcomes_.size()) > config_.window_size) {
        outcomes_.pop_front();
    }

    if (state_ == State::HALF_OPEN) {
        // Probe failed -> reopen the circuit with a fresh cooldown.
        state_ = State::OPEN;
        opened_at_ = std::chrono::steady_clock::now();
        probe_in_flight_ = false;
        return;  // No need to EvaluateState; we already set OPEN.
    }

    EvaluateState();
}

CircuitBreaker::State CircuitBreaker::state() const {
    std::lock_guard lock(mutex_);
    return state_;
}

double CircuitBreaker::error_rate() const {
    std::lock_guard lock(mutex_);
    if (outcomes_.empty()) return 0.0;
    int failures = 0;
    for (bool ok : outcomes_) {
        if (!ok) failures++;
    }
    return static_cast<double>(failures) / outcomes_.size();
}

// ---------------------------------------------------------------------------
// EvaluateState: check whether the sliding window error rate exceeds the
// threshold and, if so, open the circuit.
//
// This only handles the CLOSED -> OPEN transition. The other transitions
// (OPEN -> HALF_OPEN, HALF_OPEN -> CLOSED/OPEN) are handled inline in
// AllowRequest and RecordSuccess/RecordFailure respectively.
//
// Guard: we require a full window (outcomes_.size() == window_size) before
// tripping. This avoids false positives during startup when the first 1-2
// requests might fail due to cold-start latency rather than true degradation.
// ---------------------------------------------------------------------------
void CircuitBreaker::EvaluateState() {
    if (state_ != State::CLOSED) return;
    if (static_cast<int>(outcomes_.size()) < config_.window_size) return;

    int failures = 0;
    for (bool ok : outcomes_) {
        if (!ok) failures++;
    }

    double rate = static_cast<double>(failures) / outcomes_.size();
    if (rate > config_.error_threshold) {
        state_ = State::OPEN;
        opened_at_ = std::chrono::steady_clock::now();
    }
}

// ============================================================================
// CircuitBreakerManager: per-replica breaker lookup with lazy initialization.
//
// The manager mutex is held for the duration of each public method call.
// This is acceptable because the critical section is short (map lookup +
// delegation to the individual CircuitBreaker, which also holds its own
// mutex briefly). For higher concurrency, the manager mutex could be
// replaced with a concurrent map, but at the expected replica count
// (tens, not thousands) this is not a bottleneck.
// ============================================================================

CircuitBreakerManager::CircuitBreakerManager() : config_({}) {}
CircuitBreakerManager::CircuitBreakerManager(CircuitBreaker::Config config)
    : config_(config) {}

bool CircuitBreakerManager::AllowRequest(const std::string& replica_id) {
    std::lock_guard lock(mutex_);
    return GetOrCreate(replica_id).AllowRequest();
}

void CircuitBreakerManager::RecordSuccess(const std::string& replica_id) {
    std::lock_guard lock(mutex_);
    GetOrCreate(replica_id).RecordSuccess();
}

void CircuitBreakerManager::RecordFailure(const std::string& replica_id) {
    std::lock_guard lock(mutex_);
    GetOrCreate(replica_id).RecordFailure();
}

CircuitBreaker::State CircuitBreakerManager::GetState(
    const std::string& replica_id) {
    std::lock_guard lock(mutex_);
    return GetOrCreate(replica_id).state();
}

// GetOrCreate: lazy factory. A breaker is created on first access with the
// shared Config. This means newly discovered replicas (via gossip) get a
// circuit breaker automatically on their first routed request, with no
// explicit registration step required.
CircuitBreaker& CircuitBreakerManager::GetOrCreate(
    const std::string& replica_id) {
    auto it = breakers_.find(replica_id);
    if (it == breakers_.end()) {
        it = breakers_.emplace(replica_id, config_).first;
    }
    return it->second;
}

}  // namespace llmgateway
