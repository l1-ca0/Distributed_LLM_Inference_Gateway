#pragma once

#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>
#include "llmgateway.grpc.pb.h"

#include "gateway/circuit_breaker.h"
#include "gateway/load_balancer.h"
#include "gateway/replica_registry.h"
#include "gateway/request_queue.h"
#include "gossip/swim.h"

namespace llmgateway {

// ---------------------------------------------------------------------------
// GatewayServer: the client-facing gRPC endpoint for inference requests.
//
// Role -- streaming reverse proxy:
//   The gateway does not run any model itself. It accepts a client's
//   InferRequest (prompt + max_tokens), selects a backend replica via the
//   LoadBalancer, opens a server-streaming Generate RPC to that replica,
//   and forwards each generated token back to the client as an
//   InferResponse. From the client's perspective, the gateway is a single
//   stable endpoint; the actual replica serving the request is transparent.
//
// Mid-stream failover (DR4):
//   If the backend replica fails or disconnects during streaming, the
//   gateway does NOT return an error immediately. Instead, it selects a
//   new replica via the load balancer and continues the stream from where
//   it left off (using tokens_already_generated to tell the new replica
//   how many tokens were already produced). The client sees a seamless
//   token stream, possibly from multiple replicas. Up to max_retries
//   failover attempts are made before giving up.
//
// Integration with other components:
//   - ReplicaRegistry: provides the set of known replicas and their state.
//   - LoadBalancer: selects which replica to route each request to.
//   - CircuitBreakerManager: gates requests to replicas that are returning
//     errors (application-level health, complementing gossip-level health).
// ---------------------------------------------------------------------------
class GatewayServer final : public InferenceGateway::Service {
public:
    GatewayServer(int port, ReplicaRegistry& registry, LoadBalancer& lb,
                  CircuitBreakerManager& cb_manager, RequestQueue& queue);

    void Start();
    void Stop();

    int port() const { return port_; }

private:
    // gRPC service handler: proxy streaming inference request to a replica.
    // Implements the mid-stream failover retry loop.
    grpc::Status Infer(grpc::ServerContext* context,
                       const InferRequest* request,
                       grpc::ServerWriter<InferResponse>* writer) override;

    // Handle a hedged request: send to 2 replicas simultaneously, stream
    // from whichever produces the first token faster, cancel the slower one.
    grpc::Status InferHedged(grpc::ServerContext* context,
                             const InferRequest* request,
                             grpc::ServerWriter<InferResponse>* writer);

    // Forward a Generate request to a specific replica and stream tokens back.
    // Returns:
    //   >= 0 : number of tokens successfully streamed to the client.
    //     -1 : the replica stream failed (gRPC error). The caller should
    //          record a circuit breaker failure and retry with another replica.
    // The -1 sentinel distinguishes "replica error" from "zero tokens streamed
    // successfully", which matters for the retry/failover decision.
    int StreamFromReplica(const std::string& replica_id,
                          const std::string& grpc_address,
                          const std::string& prompt,
                          int max_tokens,
                          int tokens_already_generated,
                          grpc::ServerWriter<InferResponse>* writer,
                          grpc::ServerContext* client_context);

    int port_;
    ReplicaRegistry& registry_;
    LoadBalancer& lb_;
    CircuitBreakerManager& cb_manager_;
    RequestQueue& queue_;
    std::unique_ptr<grpc::Server> server_;
};

}  // namespace llmgateway
