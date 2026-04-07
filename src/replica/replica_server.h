#pragma once

#include <atomic>
#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>
#include "llmgateway.grpc.pb.h"

namespace llmgateway {

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

private:
    // gRPC service handlers
    grpc::Status Generate(grpc::ServerContext* context,
                          const GenerateRequest* request,
                          grpc::ServerWriter<GenerateResponse>* writer) override;

    grpc::Status Drain(grpc::ServerContext* context,
                       const DrainRequest* request,
                       DrainResponse* response) override;

    std::string replica_id_;
    std::string model_version_ = "v1";
    int port_;
    int token_delay_ms_;
    int max_capacity_;
    std::atomic<int> active_requests_{0};
    std::atomic<bool> draining_{false};
    std::unique_ptr<grpc::Server> server_;
};

}  // namespace llmgateway
