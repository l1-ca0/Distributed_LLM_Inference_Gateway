#pragma once

#include <functional>
#include <future>
#include <string>
#include <vector>

#include <grpcpp/grpcpp.h>
#include "llmgateway.grpc.pb.h"

namespace llmgateway {

struct InferResult {
    std::vector<std::string> tokens;
    std::vector<std::string> replica_ids;  // which replica(s) served this request
    bool success = false;
    std::string error_message;
    int request_index = -1;  // for tracking order in concurrent batches
};

// Callback invoked for each token as it arrives during streaming.
// Parameters: token string, replica_id, token index (0-based).
// Return false to cancel the stream.
using TokenCallback = std::function<bool(const std::string& token,
                                         const std::string& replica_id,
                                         int token_index)>;

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
