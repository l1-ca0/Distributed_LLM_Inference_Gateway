#pragma once

// ---------------------------------------------------------------------------
// Port allocator for test cluster setup.
//
// Tests need unique ports for every replica + gateway to avoid collisions:
//   - Between concurrent test runs
//   - With ports left in TIME_WAIT from previous runs
//   - With other processes on the same machine
//
// Strategy: each call to Allocate() returns the next port in a high range
// (default: 20000+). The counter is thread-safe (atomic). We don't actually
// verify the port is free — we just increment monotonically. For test
// reliability, always pair this with SO_REUSEADDR on the actual sockets,
// which both UdpTransport and gRPC do.
// ---------------------------------------------------------------------------

#include <atomic>
#include <cstdint>

namespace llmgateway {

class TestPortAllocator {
public:
    // Allocate the next free port.
    static uint16_t Allocate() {
        return next_port_.fetch_add(1);
    }

    // Reset the counter (only useful at the very start of the test suite).
    // Not thread-safe — call before any tests run.
    static void Reset(uint16_t start_port = 20000) {
        next_port_.store(start_port);
    }

private:
    // Start at 20000 — well above the typical ephemeral port range (32768+)
    // and the ports used in SWIM integration tests (18xxx).
    static inline std::atomic<uint16_t> next_port_{20000};
};

}  // namespace llmgateway
