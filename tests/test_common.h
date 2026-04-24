#pragma once

// ---------------------------------------------------------------------------
// Shared test utilities.
//
// Every test file includes this header to get:
//   - TEST(name): run a test function, catch exceptions, print PASS/FAIL
//   - ASSERT(cond, msg): check a condition, throw with file:line on failure
//   - wait_for(pred, timeout): poll a predicate with timeout (for async tests)
//
// Test output (pass/fail + points) goes to stdout. Component logs
// (LOG_INFO etc.) go to stderr and are suppressed via Log::SetQuiet(true)
// at test startup to keep the output clean for graders.
// ---------------------------------------------------------------------------

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

// Shared counters for pass/fail tracking.
// Each test file declares its own static instances at file scope.
#define TEST_COMMON_COUNTERS()                                             \
    static int tests_passed = 0;                                           \
    static int tests_failed = 0;

#define TEST(name)                                                         \
    std::cout << "  " << #name << "... " << std::flush;                    \
    try {                                                                  \
        test_##name();                                                     \
        std::cout << "PASS" << std::endl;                                  \
        tests_passed++;                                                    \
    } catch (const std::exception& e) {                                    \
        std::cout << "FAIL: " << e.what() << std::endl;                    \
        tests_failed++;                                                    \
    }

// Assert a condition, throwing with file:line info on failure.
// The thrown exception is caught by TEST() and reported as "FAIL: <msg>".
#define ASSERT(cond, msg)                                                  \
    if (!(cond))                                                           \
        throw std::runtime_error(std::string(msg) + " [" + __FILE__ +      \
                                 ":" + std::to_string(__LINE__) + "]");

namespace llmgateway {

// Poll a predicate until it returns true or timeout expires.
// Used for async tests where we're waiting for eventual consistency
// (e.g., gossip convergence, membership state propagation).
//
// Returns true if the predicate succeeded, false on timeout.
template <typename Pred>
inline bool wait_for(Pred pred, int timeout_ms, int poll_interval_ms = 100) {
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(poll_interval_ms));
    }
    return pred();
}

}  // namespace llmgateway
