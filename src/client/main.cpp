#include <iostream>
#include <string>

#include "client/client.h"

int main(int argc, char* argv[]) {
    // Defaults
    std::string target = "localhost:50051";
    std::string prompt = "Hello, world!";
    int max_tokens = 10;
    bool direct = false;  // if true, call replica directly (bypass gateway)
    bool hedge = false;    // if true, enable request hedging

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--target" && i + 1 < argc) target = argv[++i];
        else if (arg == "--prompt" && i + 1 < argc) prompt = argv[++i];
        else if (arg == "--max-tokens" && i + 1 < argc) max_tokens = std::stoi(argv[++i]);
        else if (arg == "--direct") direct = true;
        else if (arg == "--hedge") hedge = true;
    }

    llmgateway::InferenceClient client(target);

    std::cout << "Connecting to " << target << "..." << std::endl;
    std::cout << "Prompt: \"" << prompt << "\", max_tokens: " << max_tokens << std::endl;
    std::cout << "---" << std::endl;

    llmgateway::InferResult result;
    if (direct) {
        result = client.GenerateDirect("req-1", prompt, max_tokens);
    } else {
        result = client.Infer("client-1", prompt, max_tokens, hedge);
    }

    if (result.success) {
        std::cout << "Received " << result.tokens.size() << " tokens:" << std::endl;
        for (const auto& token : result.tokens) {
            std::cout << "  " << token << std::endl;
        }
        if (!result.replica_ids.empty()) {
            std::cout << "Served by replica(s):";
            for (const auto& rid : result.replica_ids) {
                std::cout << " " << rid;
            }
            std::cout << std::endl;
        }
    } else {
        std::cerr << "Error: " << result.error_message << std::endl;
        return 1;
    }

    return 0;
}
