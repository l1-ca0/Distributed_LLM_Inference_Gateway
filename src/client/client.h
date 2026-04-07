#pragma once

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
};

class InferenceClient {
public:
    explicit InferenceClient(const std::string& target_address);

    // Blocking inference: sends request, collects all streamed tokens, returns result.
    InferResult Infer(const std::string& client_id, const std::string& prompt,
                      int max_tokens);

private:
    std::unique_ptr<InferenceGateway::Stub> stub_;

    // For direct replica testing (bypasses gateway, uses LLMReplica service)
    std::unique_ptr<LLMReplica::Stub> replica_stub_;

public:
    // Direct replica call (for testing without gateway)
    InferResult GenerateDirect(const std::string& request_id,
                               const std::string& prompt, int max_tokens,
                               int tokens_already_generated = 0);
};

}  // namespace llmgateway
