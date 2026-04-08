#include "replica/replica_server.h"

#include <chrono>
#include <random>
#include <thread>

#include "common/log.h"

namespace llmgateway {

static thread_local std::mt19937 g_rng{std::random_device{}()};

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
    LOG_INFO("replica", "%s listening on %s (delay=%dms, capacity=%d)",
             replica_id_.c_str(), server_address.c_str(), token_delay_ms_, max_capacity_);
}

void ReplicaServer::Stop() {
    if (server_) {
        server_->Shutdown();
        LOG_INFO("replica", "%s stopped", replica_id_.c_str());
    }
}

void ReplicaServer::set_fault_config(const ReplicaFaultConfig& config) {
    error_rate_.store(config.error_rate);
    reject_all_.store(config.reject_all);
}

bool ReplicaServer::ShouldInjectError() const {
    double rate = error_rate_.load();
    if (rate <= 0.0) return false;
    if (rate >= 1.0) return true;
    std::uniform_real_distribution<double> dist(0.0, 1.0);
    return dist(g_rng) < rate;
}

grpc::Status ReplicaServer::Generate(grpc::ServerContext* context,
                                     const GenerateRequest* request,
                                     grpc::ServerWriter<GenerateResponse>* writer) {
    // Reject if draining.
    if (draining_.load()) {
        return grpc::Status(grpc::StatusCode::UNAVAILABLE, "replica is draining");
    }

    // Fault injection: reject all requests.
    if (reject_all_.load()) {
        return grpc::Status(grpc::StatusCode::INTERNAL, "replica is degraded (fault injection)");
    }

    // Fault injection: probabilistic error.
    if (ShouldInjectError()) {
        return grpc::Status(grpc::StatusCode::INTERNAL, "injected error");
    }

    active_requests_.fetch_add(1);

    int start = request->tokens_already_generated();
    int end = request->max_tokens();

    for (int i = start; i < end; i++) {
        if (context->IsCancelled()) {
            active_requests_.fetch_sub(1);
            return grpc::Status::CANCELLED;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(token_delay_ms_));

        GenerateResponse response;
        response.set_token("token_" + std::to_string(i));
        response.set_is_final(i == end - 1);

        if (!writer->Write(response)) {
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
    LOG_INFO("replica", "%s draining...", replica_id_.c_str());
    draining_.store(true);

    while (active_requests_.load() > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    response->set_success(true);
    LOG_INFO("replica", "%s drained", replica_id_.c_str());
    return grpc::Status::OK;
}

}  // namespace llmgateway
