#pragma once

// Blocking gRPC client for the inference service. Primary entry point
// for tests and the standalone client binary. Supports direct-replica
// mode, hedging, per-token callbacks, and concurrent batches.

#include <functional>
#include <future>
#include <string>
#include <vector>

#include <grpcpp/grpcpp.h>
#include "llmgateway.grpc.pb.h"

namespace llmgateway {

// Result of a single inference call. The tokens and replica_ids vectors
// are index-aligned: replica_ids[i] identifies the replica that produced
// tokens[i]. Under mid-stream failover or hedging, different indices may
// reference different replicas — tests rely on this to verify routing
// behavior. When success is false, error_message explains why; the tokens
// vector may still contain a partial prefix streamed before the failure.
struct InferResult {
    std::vector<std::string> tokens;
    std::vector<std::string> replica_ids;
    bool success = false;
    std::string error_message;
    int request_index = -1;  // for tracking order in concurrent batches
};

// Invoked synchronously on the caller's thread for each token received
// during streaming. Returning false cancels the gRPC stream (via
// ClientContext::TryCancel), causing the current Infer call to return
// with success=false. Primarily used by tests to inject mid-stream
// cancellation behavior.
using TokenCallback = std::function<bool(const std::string& token,
                                         const std::string& replica_id,
                                         int token_index)>;

// ---------------------------------------------------------------------------
// InferenceClient: blocking gRPC client for the LLM inference service.
//
// Typical usage is against the gateway (which load-balances across
// replicas), but the client also exposes GenerateDirect() for bypass mode
// that talks to a replica directly — used by tests to verify per-replica
// behavior without the gateway in the loop.
//
// All Infer* methods block the caller's thread until the stream completes,
// fails, or is cancelled. Each InferenceClient owns a gRPC channel to its
// target_address; channels are thread-safe and may be reused across
// sequential calls, but concurrent calls on the same InferenceClient are
// not supported (use InferConcurrent for parallelism instead).
// ---------------------------------------------------------------------------
class InferenceClient {
public:
    explicit InferenceClient(const std::string& target_address);

    // Blocking inference: sends request, collects all streamed tokens, returns result.
    InferResult Infer(const std::string& client_id, const std::string& prompt,
                      int max_tokens, bool hedge = false);

    // Blocking inference with per-token callback (for mid-stream test control).
    // The callback is invoked for each token; returning false cancels the stream.
    InferResult InferWithCallback(const std::string& client_id,
                                  const std::string& prompt, int max_tokens,
                                  TokenCallback on_token);

    // Direct replica call (for testing without gateway).
    InferResult GenerateDirect(const std::string& request_id,
                               const std::string& prompt, int max_tokens,
                               int tokens_already_generated = 0);

    // Fire N concurrent inference requests and collect all results.
    // Each request gets a unique prompt (prompt_prefix + "_" + index).
    // Returns results in the same order as the requests were issued.
    static std::vector<InferResult> InferConcurrent(
        const std::string& target_address, int count,
        const std::string& client_id, const std::string& prompt_prefix,
        int max_tokens, bool hedge = false);

private:
    std::unique_ptr<InferenceGateway::Stub> stub_;
    std::unique_ptr<LLMReplica::Stub> replica_stub_;
};

}  // namespace llmgateway
