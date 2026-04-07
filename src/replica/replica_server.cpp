#include "replica/replica_server.h"

#include <chrono>
#include <iostream>
#include <thread>

namespace llmgateway {

ReplicaServer::ReplicaServer(const std::string& replica_id, int port,
                             int token_delay_ms, int max_capacity)
    : replica_id_(replica_id),
      port_(port),
      token_delay_ms_(token_delay_ms),
      max_capacity_(max_capacity) {}

void ReplicaServer::Start() {
    std::string server_address = "0.0.0.0:" + std::to_string(port_);

    grpc::ServerBuilder builder;
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(this);

    server_ = builder.BuildAndStart();
    std::cout << "[replica " << replica_id_ << "] listening on " << server_address
              << " (delay=" << token_delay_ms_ << "ms, capacity=" << max_capacity_
              << ")" << std::endl;
}

void ReplicaServer::Stop() {
    if (server_) {
        server_->Shutdown();
        std::cout << "[replica " << replica_id_ << "] stopped" << std::endl;
    }
}

grpc::Status ReplicaServer::Generate(grpc::ServerContext* context,
                                     const GenerateRequest* request,
                                     grpc::ServerWriter<GenerateResponse>* writer) {
    // Reject if draining
    if (draining_.load()) {
        return grpc::Status(grpc::StatusCode::UNAVAILABLE, "replica is draining");
    }

    active_requests_.fetch_add(1);

    int start = request->tokens_already_generated();
    int end = request->max_tokens();

    for (int i = start; i < end; i++) {
        // Check if client disconnected
        if (context->IsCancelled()) {
            active_requests_.fetch_sub(1);
            return grpc::Status::CANCELLED;
        }

        // Simulate token generation delay
        std::this_thread::sleep_for(std::chrono::milliseconds(token_delay_ms_));

        GenerateResponse response;
        response.set_token("token_" + std::to_string(i));
        response.set_is_final(i == end - 1);

        if (!writer->Write(response)) {
            // Client disconnected mid-stream
            active_requests_.fetch_sub(1);
            return grpc::Status::CANCELLED;
        }
    }

    active_requests_.fetch_sub(1);
    return grpc::Status::OK;
}

grpc::Status ReplicaServer::Drain(grpc::ServerContext* context,
                                  const DrainRequest* request,
                                  DrainResponse* response) {
    std::cout << "[replica " << replica_id_ << "] draining..." << std::endl;
    draining_.store(true);

    // Wait for all active requests to complete
    while (active_requests_.load() > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    response->set_success(true);
    std::cout << "[replica " << replica_id_ << "] drained" << std::endl;
    return grpc::Status::OK;
}

}  // namespace llmgateway
