#include "gateway/request_queue.h"

#include "common/log.h"

namespace llmgateway {

RequestQueue::RequestQueue(int max_size) : max_size_(max_size) {}

// ---------------------------------------------------------------------------
// WaitForCapacity: block until notified that a replica has capacity.
//
// Ticket-based FIFO:
//   Each caller takes a ticket number (next_ticket_++) under the lock.
//   The condition_variable predicate checks whether serving_ticket_ has
//   reached this thread's ticket. NotifyCapacityAvailable() increments
//   serving_ticket_ and wakes all waiters, but only the one whose ticket
//   matches will pass the predicate and proceed. This guarantees strict
//   FIFO ordering regardless of OS thread scheduling.
//
// Bounded queue:
//   If waiting_count_ >= max_size_, the request is rejected immediately.
//   This prevents the queue from growing unboundedly under sustained
//   overload. The caller should return RESOURCE_EXHAUSTED to the client.
//
// Timeout:
//   A 30-second default timeout prevents threads from blocking forever
//   if the cluster is completely dead. On timeout, the thread returns
//   false and the caller should return DEADLINE_EXCEEDED or UNAVAILABLE.
// ---------------------------------------------------------------------------
bool RequestQueue::WaitForCapacity(int timeout_ms) {
    std::unique_lock lock(mutex_);

    // Check-and-admit happen under the lock so waiting_count_ cannot
    // exceed max_size_ under concurrent admission.
    if (waiting_count_.load() >= max_size_) {
        int current = waiting_count_.load();
        LOG_WARN("queue", "queue full (%d/%d), rejecting request",
                 current, max_size_);
        return false;
    }
    waiting_count_.fetch_add(1);

    uint64_t my_ticket = next_ticket_++;

    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);

    // Wait until our ticket is being served, or timeout.
    bool served = cv_.wait_until(lock, deadline, [&]() {
        return serving_ticket_ > my_ticket;
    });

    waiting_count_.fetch_sub(1);

    if (!served) {
        LOG_WARN("queue", "request timed out after %dms in queue", timeout_ms);
    }

    return served;
}

// ---------------------------------------------------------------------------
// NotifyCapacityAvailable: called when a replica finishes a request.
//
// Increments serving_ticket_ so the next FIFO waiter's predicate becomes
// true, then wakes all waiters. Only the one whose ticket matches will
// actually proceed; the rest go back to sleep.
//
// Using notify_all() instead of notify_one() because the predicate is
// ticket-specific — notify_one() might wake a thread whose ticket isn't
// next, causing it to sleep again while the correct thread remains asleep.
// The cost is minimal (spurious wakeups are cheap; the predicate filters).
// ---------------------------------------------------------------------------
void RequestQueue::NotifyCapacityAvailable() {
    {
        std::lock_guard lock(mutex_);
        serving_ticket_++;
    }
    cv_.notify_all();
}

}  // namespace llmgateway
