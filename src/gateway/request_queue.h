#pragma once

// Bounded FIFO backpressure queue. When every replica is at capacity,
// incoming Infer calls block on this queue until a request finishes
// and capacity is notified, rather than failing immediately.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <queue>

namespace llmgateway {

// ---------------------------------------------------------------------------
// RequestQueue: FIFO backpressure queue for the inference gateway.
//
// When all replicas are at capacity, the gateway cannot immediately route
// an incoming request. Instead of rejecting it with RESOURCE_EXHAUSTED,
// the request thread blocks on this queue until a replica frees up.
//
// Flow:
//   1. Gateway's Infer() handler calls SelectReplica() and gets nullopt.
//   2. Instead of returning an error, it calls WaitForCapacity() which
//      blocks the calling thread on a condition_variable.
//   3. When any in-flight streaming session completes, the gateway calls
//      NotifyCapacityAvailable(), which wakes one waiting thread.
//   4. The woken thread retries SelectReplica() — it may succeed now.
//
// The queue is bounded (max_size_). If the queue is full when a new request
// arrives, WaitForCapacity() returns false immediately, and the gateway
// returns RESOURCE_EXHAUSTED. This prevents unbounded memory growth under
// sustained overload.
//
// FIFO ordering: waiting threads are woken in the order they called
// WaitForCapacity(), because condition_variable::notify_one() wakes the
// thread that has been waiting the longest (in practice, on most
// implementations). For strict FIFO, we use a ticket-based system.
// ---------------------------------------------------------------------------
class RequestQueue {
public:
    explicit RequestQueue(int max_size = 100);

    // Block the calling thread until capacity is available or timeout expires.
    // Returns true if the thread was woken by NotifyCapacityAvailable().
    // Returns false if the queue is full (overload) or timeout expired.
    bool WaitForCapacity(int timeout_ms = 30000);

    // Wake one waiting thread (called when a replica finishes a request).
    void NotifyCapacityAvailable();

    // Current number of threads waiting in the queue.
    int waiting_count() const { return waiting_count_.load(); }

    // Maximum queue size.
    int max_size() const { return max_size_; }

private:
    int max_size_;
    std::atomic<int> waiting_count_{0};

    std::mutex mutex_;
    std::condition_variable cv_;

    // Ticket system for strict FIFO ordering.
    // Each waiting thread takes a ticket; NotifyCapacityAvailable increments
    // the serving counter. A thread wakes only when its ticket is being served.
    uint64_t next_ticket_ = 0;
    uint64_t serving_ticket_ = 0;
};

}  // namespace llmgateway
