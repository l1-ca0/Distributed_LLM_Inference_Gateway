// test suite
//
// Each test lives in its own .cpp and returns a TestResult.
// This driver invokes them in sequence, prints a
// per-test PASS/FAIL line with points earned.
//
// Tests are independent: each constructs its own TestCluster on unique
// ports handed out by TestPortAllocator.

#include <iostream>
#include <vector>

#include "common/log.h"
#include "tests/test_runner.h"

namespace llmgateway {
TestResult test1_load_balancing();
TestResult test2_gossip_failure_detection();
TestResult test3_gossip_convergence();
TestResult test4_midstream_failover();
TestResult test5_backpressure();
TestResult test6_suspicion_refutation();
TestResult test7_rolling_update();
TestResult test8_circuit_breaker();
TestResult test9_request_hedging();
}  // namespace llmgateway

using namespace llmgateway;

int main() {
    Log::SetLevel(LogLevel::WARN);

    std::cout << "=== Distributed LLM Inference Gateway — Test Suite ===\n"
              << std::endl;

    std::vector<TestResult> results;
    results.push_back(test1_load_balancing());
    print_result(results.back());

    results.push_back(test2_gossip_failure_detection());
    print_result(results.back());

    results.push_back(test3_gossip_convergence());
    print_result(results.back());

    results.push_back(test4_midstream_failover());
    print_result(results.back());

    results.push_back(test5_backpressure());
    print_result(results.back());

    results.push_back(test6_suspicion_refutation());
    print_result(results.back());

    results.push_back(test7_rolling_update());
    print_result(results.back());

    results.push_back(test8_circuit_breaker());
    print_result(results.back());

    results.push_back(test9_request_hedging());
    print_result(results.back());

    print_summary(results);

    // The printed TOTAL line is the grading signal; the process exit
    // code is not used.
    return 0;
}
