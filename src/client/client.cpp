#include "client/client.h"

#include <iostream>
#include <thread>

namespace llmgateway {

InferenceClient::InferenceClient(const std::string& target_address) {
    auto channel = grpc::CreateChannel(target_address,
                                       grpc::InsecureChannelCredentials());
    stub_ = InferenceGateway::NewStub(channel);
    replica_stub_ = LLMReplica::NewStub(channel);
}

InferResult InferenceClient::Infer(const std::string& client_id,
                                   const std::string& prompt, int max_tokens,
                                   bool hedge) {
    InferResult result;

    InferRequest request;
    request.set_client_id(client_id);
    request.set_prompt(prompt);
    request.set_max_tokens(max_tokens);
    request.set_hedge(hedge);

    grpc::ClientContext context;
    auto reader = stub_->Infer(&context, request);

    InferResponse response;
    while (reader->Read(&response)) {
        result.tokens.push_back(response.token());
        if (!response.replica_id().empty()) {
            if (result.replica_ids.empty() ||
                result.replica_ids.back() != response.replica_id()) {
                result.replica_ids.push_back(response.replica_id());
            }
        }
    }

    grpc::Status status = reader->Finish();
    result.success = status.ok();
    if (!result.success) {
        result.error_message = status.error_message();
    }

    return result;
}

InferResult InferenceClient::InferWithCallback(const std::string& client_id,
                                               const std::string& prompt,
                                               int max_tokens,
                                               TokenCallback on_token) {
    InferResult result;

    InferRequest request;
    request.set_client_id(client_id);
    request.set_prompt(prompt);
    request.set_max_tokens(max_tokens);

    grpc::ClientContext context;
    auto reader = stub_->Infer(&context, request);

    InferResponse response;
    int token_index = 0;
    while (reader->Read(&response)) {
        result.tokens.push_back(response.token());
        if (!response.replica_id().empty()) {
            if (result.replica_ids.empty() ||
                result.replica_ids.back() != response.replica_id()) {
                result.replica_ids.push_back(response.replica_id());
            }
        }

        // Invoke callback; if it returns false, cancel the stream.
        if (on_token && !on_token(response.token(), response.replica_id(), token_index)) {
            context.TryCancel();
            break;
        }
        token_index++;
    }

    grpc::Status status = reader->Finish();
    result.success = status.ok() || status.error_code() == grpc::StatusCode::CANCELLED;
    if (!result.success) {
        result.error_message = status.error_message();
    }

    return result;
}

InferResult InferenceClient::GenerateDirect(const std::string& request_id,
                                            const std::string& prompt,
                                            int max_tokens,
                                            int tokens_already_generated) {
    InferResult result;

    GenerateRequest request;
    request.set_request_id(request_id);
    request.set_prompt(prompt);
    request.set_max_tokens(max_tokens);
    request.set_tokens_already_generated(tokens_already_generated);

    grpc::ClientContext context;
    auto reader = replica_stub_->Generate(&context, request);

    GenerateResponse response;
    while (reader->Read(&response)) {
        result.tokens.push_back(response.token());
    }

    grpc::Status status = reader->Finish();
    result.success = status.ok();
    if (!result.success) {
        result.error_message = status.error_message();
    }

    return result;
}

std::vector<InferResult> InferenceClient::InferConcurrent(
    const std::string& target_address, int count,
    const std::string& client_id, const std::string& prompt_prefix,
    int max_tokens, bool hedge) {

    std::vector<InferResult> results(count);
    std::vector<std::thread> threads;
    threads.reserve(count);

    for (int i = 0; i < count; i++) {
        threads.emplace_back([&, i]() {
            // Each thread creates its own client (own gRPC channel).
            InferenceClient client(target_address);
            std::string prompt = prompt_prefix + "_" + std::to_string(i);
            results[i] = client.Infer(client_id, prompt, max_tokens, hedge);
            results[i].request_index = i;
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    return results;
}

}  // namespace llmgateway
