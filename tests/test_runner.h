#pragma once

// ---------------------------------------------------------------------------
// Test harness for the integration test suite.
//
// Each graded test is a function returning a TestResult. The driver calls
// them in sequence, aggregates results, and prints a summary.
//
// Design:
//   - Single driver binary so the autograder
//     invokes one command. Tests are independent because each constructs a
//     fresh TestCluster and the port allocator hands out unique ports
//     process-wide.
//   - Tests signal failure by throwing; the runner catches and records the
//     exception message in TestResult::error. ASSERT() from test_common.h
//     throws std::runtime_error with file:line info.
//   - Partial credit is not supported. A test either meets its full
//     specification or earns 0 points.
// ---------------------------------------------------------------------------

#include <chrono>
#include <exception>
#include <functional>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace llmgateway {

struct TestResult {
    std::string name;
    int points_earned = 0;
    int points_max = 0;
    bool passed = false;
    std::string error;
    long duration_ms = 0;
};

// Run a named test. On exception, the result is marked failed with the
// exception message captured in `error`. Duration is measured on every run
// so the summary can flag slow tests.
inline TestResult run_test(const std::string& name, int max_points,
                           const std::function<void()>& fn) {
    TestResult r;
    r.name = name;
    r.points_max = max_points;

    auto t0 = std::chrono::steady_clock::now();
    try {
        fn();
        r.passed = true;
        r.points_earned = max_points;
    } catch (const std::exception& e) {
        r.error = e.what();
    } catch (...) {
        r.error = "unknown exception";
    }
    r.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    return r;
}

inline void print_result(const TestResult& r) {
    std::cout << "  " << std::left << std::setw(52) << r.name
              << " ... " << (r.passed ? "PASS" : "FAIL")
              << " (" << r.points_earned << "/" << r.points_max << ")"
              << "  [" << r.duration_ms << "ms]";
    if (!r.passed && !r.error.empty()) {
        std::cout << "\n      -> " << r.error;
    }
    std::cout << std::endl;
}

inline void print_summary(const std::vector<TestResult>& results) {
    int total_max = 0, total_earned = 0, passed = 0;
    for (const auto& r : results) {
        total_max += r.points_max;
        total_earned += r.points_earned;
        if (r.passed) ++passed;
    }

    std::cout << "\n=== Results ===\n";
    std::cout << "  Tests passed: " << passed << "/" << results.size() << "\n";
    std::cout << "=== TOTAL: " << total_earned << "/" << total_max << " ==="
              << std::endl;
}

}  // namespace llmgateway
