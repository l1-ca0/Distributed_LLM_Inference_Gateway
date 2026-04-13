#include "gateway/gateway_server.h"

#include <chrono>

#include "common/log.h"

namespace llmgateway {

GatewayServer::GatewayServer(int port, ReplicaRegistry& registry,
                             LoadBalancer& lb, CircuitBreakerManager& cb_manager)
    : port_(port), registry_(registry), lb_(lb), cb_manager_(cb_manager) {}

void GatewayServer::Start() {
    std::string server_address = "0.0.0.0:" + std::to_string(port_);

    grpc::ServerBuilder builder;
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(this);

    server_ = builder.BuildAndStart();
    LOG_INFO("gateway", "listening on %s", server_address.c_str());
}

void GatewayServer::Stop() {
    if (server_) {
        server_->Shutdown();
        LOG_INFO("gateway", "stopped");
    }
}

// ---------------------------------------------------------------------------
// Infer: the main request handler. This is the hot path for every client
// inference call and implements the mid-stream failover strategy.
//
// Retry loop design:
//   The loop runs up to max_retries times. Each iteration:
//     1. Check if the client cancelled (e.g., timeout, user abort).
//     2. Ask the LoadBalancer for a replica (consistent hash -> least-conn).
//     3. Check the circuit breaker. If the selected replica's circuit is
//        open, skip it and burn one retry iteration (continue). The load
//        balancer may return the same replica again, but the circuit
//        breaker prevents hammering it.
//     4. Increment the active request counter (for load-balancer awareness),
//        then stream tokens from the replica.
//     5. On success (streamed >= 0): accumulate tokens_sent, record a
//        circuit-breaker success. If all tokens are delivered, return OK.
//     6. On failure (streamed == -1): record a circuit-breaker failure and
//        retry with a (potentially different) replica. tokens_sent is NOT
//        incremented, so the next replica picks up from where we left off.
//
// Partial delivery: if some tokens were sent before all retries exhausted,
// return OK rather than an error. The client already received partial
// output on the stream, so an error status would be confusing. The client
// can detect incompleteness by checking whether is_final was ever true.
//
// RESOURCE_EXHAUSTED: returned when no replica has capacity at all (before
// any retry). This signals the client to back off or queue locally.
// ---------------------------------------------------------------------------
grpc::Status GatewayServer::Infer(grpc::ServerContext* context,
                                  const InferRequest* request,
                                  grpc::ServerWriter<InferResponse>* writer) {
    const int max_tokens = request->max_tokens();
    const std::string& prompt = request->prompt();
    int tokens_sent = 0;
    int max_retries = 3;

    for (int attempt = 0; attempt < max_retries && tokens_sent < max_tokens; attempt++) {
        if (context->IsCancelled()) {
            return grpc::Status::CANCELLED;
        }

        auto selection = lb_.SelectReplica(prompt);
        if (!selection) {
            return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                                "all replicas at capacity");
        }

        // Circuit breaker check: if the replica is in OPEN state, skip it.
        // This consumes a retry attempt, which is intentional -- it gives
        // the load balancer a chance to pick a different replica next time.
        if (!cb_manager_.AllowRequest(selection->replica_id)) {
            LOG_WARN("gateway", "circuit open for %s, trying next",
                     selection->replica_id.c_str());
            continue;
        }

        // Track this in-flight request so the load balancer accounts for it.
        registry_.IncrementActive(selection->replica_id);

        int streamed = StreamFromReplica(
            selection->replica_id, selection->grpc_address,
            prompt, max_tokens, tokens_sent, writer, context);

        registry_.DecrementActive(selection->replica_id);

        if (streamed < 0) {
            // streamed == -1 signals a replica-side gRPC error. Record the
            // failure so the circuit breaker can track the error rate and
            // potentially open the circuit for this replica.
            cb_manager_.RecordFailure(selection->replica_id);
            LOG_WARN("gateway", "replica %s failed after %d tokens, retrying",
                     selection->replica_id.c_str(), tokens_sent);
            continue;
        }

        tokens_sent += streamed;
        cb_manager_.RecordSuccess(selection->replica_id);

        if (tokens_sent >= max_tokens) {
            return grpc::Status::OK;
        }

        // Partial stream (streamed > 0 but < remaining): the replica ended
        // early. Continue the retry loop to pick up from tokens_sent.
    }

    if (tokens_sent >= max_tokens) {
        return grpc::Status::OK;
    }

    if (tokens_sent > 0) {
        // Partial delivery: some tokens were already written to the stream.
        // Returning an error here would be misleading since the client
        // already received data. Return OK and let the client detect
        // incompleteness via the absence of is_final=true.
        LOG_WARN("gateway", "partial delivery: %d/%d tokens", tokens_sent, max_tokens);
        return grpc::Status::OK;
    }

    return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                        "all replicas failed after retries");
}

// ---------------------------------------------------------------------------
// StreamFromReplica: the core streaming proxy logic.
//
// Flow:
//   1. Create a gRPC channel+stub to the replica (one per call; channels
//      are lightweight and cached internally by gRPC's channel pool).
//   2. Build a GenerateRequest with the original prompt, max_tokens, and
//      tokens_already_generated. The replica uses tokens_already_generated
//      to skip tokens that were already sent to the client by a previous
//      replica (mid-stream failover).
//   3. Open a server-streaming RPC (Generate) and read tokens in a loop.
//   4. For each token: check if the client cancelled, then forward the
//      token as an InferResponse on the client-facing stream. If the
//      client disconnected (Write returns false), cancel the replica
//      stream to avoid wasting GPU cycles.
//   5. After the Read loop ends, call Finish() to get the final gRPC
//      status from the replica.
//
// Return value convention:
//   >= 0 : number of tokens successfully forwarded to the client.
//     -1 : replica-side error (the gRPC stream terminated with a non-OK
//          status). The caller (Infer) uses -1 to distinguish "zero
//          tokens streamed successfully" from "stream failed before any
//          token". This distinction drives the circuit-breaker recording:
//          -1 triggers RecordFailure; 0 does not.
// ---------------------------------------------------------------------------
int GatewayServer::StreamFromReplica(const std::string& replica_id,
                                     const std::string& grpc_address,
                                     const std::string& prompt,
                                     int max_tokens,
                                     int tokens_already_generated,
                                     grpc::ServerWriter<InferResponse>* writer,
                                     grpc::ServerContext* client_context) {
    auto channel = grpc::CreateChannel(grpc_address,
                                       grpc::InsecureChannelCredentials());
    auto stub = LLMReplica::NewStub(channel);

    GenerateRequest gen_request;
    gen_request.set_request_id("req-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    gen_request.set_prompt(prompt);
    gen_request.set_max_tokens(max_tokens);
    gen_request.set_tokens_already_generated(tokens_already_generated);

    grpc::ClientContext replica_context;
    auto reader = stub->Generate(&replica_context, gen_request);

    int tokens_streamed = 0;
    GenerateResponse gen_response;

    while (reader->Read(&gen_response)) {
        // Early exit if the downstream client has gone away.
        if (client_context->IsCancelled()) {
            replica_context.TryCancel();
            return tokens_streamed;
        }

        // Translate the replica's GenerateResponse into the client-facing
        // InferResponse, tagging it with the serving replica_id for
        // observability (the client can see which replica served each token).
        InferResponse infer_response;
        infer_response.set_token(gen_response.token());
        infer_response.set_is_final(gen_response.is_final());
        infer_response.set_replica_id(replica_id);

        if (!writer->Write(infer_response)) {
            // Client disconnected mid-stream. Cancel the replica RPC to
            // free up its resources (GPU memory, request slot).
            replica_context.TryCancel();
            return tokens_streamed;
        }

        tokens_streamed++;
    }

    // Finish() retrieves the final status from the replica.
    // A non-OK status means the replica encountered an error (crash,
    // timeout, OOM, etc.). We return -1 to signal failure so the Infer
    // retry loop can attempt failover to another replica.
    grpc::Status status = reader->Finish();
    if (!status.ok()) {
        LOG_WARN("gateway", "stream from %s failed: %s",
                 replica_id.c_str(), status.error_message().c_str());
        return -1;
    }

    return tokens_streamed;
}

}  // namespace llmgateway
