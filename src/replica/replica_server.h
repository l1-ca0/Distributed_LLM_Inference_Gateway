#pragma once

#include <atomic>
#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>
#include "llmgateway.grpc.pb.h"

namespace llmgateway {

// Configuration for simulating replica failure modes.
// Used by tests to trigger circuit breaker, test failover, etc.
struct ReplicaFaultConfig {
    // Fraction of Generate requests that return an error (0.0 = none, 1.0 = all).
    // The replica remains alive and responsive to gossip — only inference fails.
    double error_rate = 0.0;

    // If true, all Generate requests return UNAVAILABLE immediately.
    // Simulates a fully degraded replica (for circuit breaker testing).
    bool reject_all = false;
};

class ReplicaServer final : public LLMReplica::Service {
public:
    ReplicaServer(const std::string& replica_id, int port, int token_delay_ms,
                  int max_capacity = 4);

    void Start();
    void Stop();

    int active_requests() const { return active_requests_.load(); }
    int max_capacity() const { return max_capacity_; }
    const std::string& replica_id() const { return replica_id_; }
    const std::string& model_version() const { return model_version_; }
    void set_model_version(const std::string& version) { model_version_ = version; }

    // Fault injection: update at runtime (thread-safe via atomics).
    void set_fault_config(const ReplicaFaultConfig& config);
    void set_error_rate(double rate) { error_rate_.store(rate); }
    void set_reject_all(bool reject) { reject_all_.store(reject); }

private:
    grpc::Status Generate(grpc::ServerContext* context,
                          const GenerateRequest* request,
                          grpc::ServerWriter<GenerateResponse>* writer) override;

    grpc::Status Drain(grpc::ServerContext* context,
                       const DrainRequest* request,
                       DrainResponse* response) override;

    // Returns true if this request should fail (based on error_rate).
    bool ShouldInjectError() const;

    std::string replica_id_;
    std::string model_version_ = "v1";
    int port_;
    int token_delay_ms_;
    int max_capacity_;
    std::atomic<int> active_requests_{0};
    std::atomic<bool> draining_{false};
    std::unique_ptr<grpc::Server> server_;

    // Fault injection state (atomics for thread-safe runtime updates).
    std::atomic<double> error_rate_{0.0};
    std::atomic<bool> reject_all_{false};
};

}  // namespace llmgateway
