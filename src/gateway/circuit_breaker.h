#pragma once

// Application-level health guard for replicas. Complements SWIM gossip
// (which catches crashes) by catching "gray" failures where a replica
// is alive on the network but failing inference requests.

#include <chrono>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>

namespace llmgateway {

// ---------------------------------------------------------------------------
// CircuitBreaker: per-replica health guard using the standard three-state
// circuit breaker pattern (Nygard, "Release It!").
//
// State machine:
//
//   CLOSED  --[error_rate > threshold]--> OPEN
//   OPEN    --[cooldown elapsed]--------> HALF_OPEN
//   HALF_OPEN --[probe succeeds]--------> CLOSED
//   HALF_OPEN --[probe fails]-----------> OPEN
//
// States explained:
//   CLOSED:    Normal operation. All requests are allowed through.
//              Outcomes are recorded in a sliding window. If the failure
//              rate within the window exceeds error_threshold, the circuit
//              transitions to OPEN.
//
//   OPEN:      The replica is considered degraded. All requests are blocked
//              (AllowRequest returns false) for cooldown_ms milliseconds.
//              After the cooldown, the next AllowRequest call transitions
//              to HALF_OPEN and lets one probe through.
//
//   HALF_OPEN: Exactly one "probe" request is in flight. If it succeeds,
//              the circuit returns to CLOSED (the replica has recovered).
//              If it fails, the circuit reopens (back to OPEN) with a
//              fresh cooldown timer.
//
// Why this complements gossip (SWIM):
//   Gossip detects binary failure (crash, network partition) via ping/ack
//   timeouts. But a replica can be alive at the gossip level while still
//   failing at the application level (e.g., OOM on large prompts, GPU
//   errors, model loading issues). The circuit breaker detects this
//   "gray failure" by tracking actual request outcomes, and temporarily
//   removes the replica from the routing pool without waiting for gossip
//   to declare it DEAD.
//
// Sliding window:
//   outcomes_ is a bounded deque of the last window_size request results
//   (true = success, false = failure). The window provides recency: old
//   failures are forgotten as new outcomes push them out, so a replica
//   that recovers will naturally close its circuit.
// ---------------------------------------------------------------------------
class CircuitBreaker {
public:
    enum class State { CLOSED, OPEN, HALF_OPEN };

    struct Config {
        int window_size = 10;        // number of recent outcomes to track
        double error_threshold = 0.5; // open circuit when error rate exceeds this
        int cooldown_ms = 5000;       // time in OPEN before transitioning to HALF_OPEN
    };

    CircuitBreaker();
    explicit CircuitBreaker(Config config);

    // Check if a request should be allowed through.
    // Returns false if the circuit is OPEN (replica is degraded).
    // In HALF_OPEN state, allows one probe request through.
    bool AllowRequest();

    // Record the outcome of a request (called after completion).
    void RecordSuccess();
    void RecordFailure();

    State state() const;
    double error_rate() const;

private:
    // Check whether the CLOSED -> OPEN transition should fire.
    // Only called from RecordSuccess/RecordFailure while holding mutex_.
    void EvaluateState();

    Config config_;
    mutable std::mutex mutex_;

    State state_ = State::CLOSED;
    std::deque<bool> outcomes_;  // sliding window: true = success, false = failure
    std::chrono::steady_clock::time_point opened_at_;  // when the circuit entered OPEN
    bool probe_in_flight_ = false;  // guards against multiple probes in HALF_OPEN
};

// ---------------------------------------------------------------------------
// CircuitBreakerManager: facade that maps replica_id -> CircuitBreaker.
//
// Circuit breakers are created lazily on first access (GetOrCreate).
// All share the same Config, but each tracks its own sliding window and
// state independently. The manager's mutex protects the map structure
// (insert/lookup), while each CircuitBreaker has its own internal mutex
// for state transitions. This means checking/recording for different
// replicas does not contend.
// ---------------------------------------------------------------------------
class CircuitBreakerManager {
public:
    CircuitBreakerManager();
    explicit CircuitBreakerManager(CircuitBreaker::Config config);

    bool AllowRequest(const std::string& replica_id);
    void RecordSuccess(const std::string& replica_id);
    void RecordFailure(const std::string& replica_id);

    CircuitBreaker::State GetState(const std::string& replica_id);

private:
    CircuitBreaker::Config config_;
    std::mutex mutex_;  // protects the breakers_ map, not individual breakers
    std::unordered_map<std::string, CircuitBreaker> breakers_;

    // Returns a reference to the breaker for replica_id, creating one with
    // the shared config_ if it does not yet exist.
    CircuitBreaker& GetOrCreate(const std::string& replica_id);
};

}  // namespace llmgateway
