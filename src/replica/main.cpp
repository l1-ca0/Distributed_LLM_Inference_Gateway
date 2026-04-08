#include <csignal>
#include <iostream>
#include <string>

#include "replica/replica_server.h"

static llmgateway::ReplicaServer* g_server = nullptr;

void signal_handler(int signal) {
    if (g_server) {
        g_server->Stop();
    }
}

int main(int argc, char* argv[]) {
    // Defaults
    std::string id = "replica-1";
    int port = 50051;
    int token_delay_ms = 100;
    int max_capacity = 4;
    std::string model_version = "v1";
    double error_rate = 0.0;
    bool reject_all = false;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--id" && i + 1 < argc) id = argv[++i];
        else if (arg == "--port" && i + 1 < argc) port = std::stoi(argv[++i]);
        else if (arg == "--token-delay-ms" && i + 1 < argc) token_delay_ms = std::stoi(argv[++i]);
        else if (arg == "--max-capacity" && i + 1 < argc) max_capacity = std::stoi(argv[++i]);
        else if (arg == "--model-version" && i + 1 < argc) model_version = argv[++i];
        else if (arg == "--error-rate" && i + 1 < argc) error_rate = std::stod(argv[++i]);
        else if (arg == "--reject-all") reject_all = true;
    }

    llmgateway::ReplicaServer server(id, port, token_delay_ms, max_capacity);
    server.set_model_version(model_version);
    server.set_error_rate(error_rate);
    server.set_reject_all(reject_all);
    g_server = &server;

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    server.Start();

    // Block until shutdown
    std::cout << "Press Ctrl+C to stop." << std::endl;
    pause();  // wait for signal

    return 0;
}
