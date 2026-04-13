#include <csignal>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "common/log.h"
#include "gossip/swim.h"
#include "replica/replica_server.h"

using namespace llmgateway;
using namespace llmgateway::gossip;

static ReplicaServer* g_server = nullptr;
static SwimProtocol* g_swim = nullptr;

void signal_handler(int signal) {
    if (g_swim) g_swim->Stop();
    if (g_server) g_server->Stop();
}

std::vector<std::string> ParseSeeds(const std::string& seeds_str) {
    std::vector<std::string> seeds;
    std::istringstream stream(seeds_str);
    std::string addr;
    while (std::getline(stream, addr, ',')) {
        if (!addr.empty()) seeds.push_back(addr);
    }
    return seeds;
}

int main(int argc, char* argv[]) {
    // Defaults
    std::string id = "replica-1";
    int grpc_port = 50051;
    int gossip_port = 0;          // 0 = grpc_port + 10000
    int token_delay_ms = 100;
    int max_capacity = 4;
    std::string model_version = "v1";
    double error_rate = 0.0;
    bool reject_all = false;
    std::string seeds_str;
    int gossip_delay_ms = 0;      // artificial delay on gossip responses (for testing)

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--id" && i + 1 < argc) id = argv[++i];
        else if (arg == "--port" && i + 1 < argc) grpc_port = std::stoi(argv[++i]);
        else if (arg == "--gossip-port" && i + 1 < argc) gossip_port = std::stoi(argv[++i]);
        else if (arg == "--token-delay-ms" && i + 1 < argc) token_delay_ms = std::stoi(argv[++i]);
        else if (arg == "--max-capacity" && i + 1 < argc) max_capacity = std::stoi(argv[++i]);
        else if (arg == "--model-version" && i + 1 < argc) model_version = argv[++i];
        else if (arg == "--error-rate" && i + 1 < argc) error_rate = std::stod(argv[++i]);
        else if (arg == "--reject-all") reject_all = true;
        else if (arg == "--seeds" && i + 1 < argc) seeds_str = argv[++i];
        else if (arg == "--gossip-delay-ms" && i + 1 < argc) gossip_delay_ms = std::stoi(argv[++i]);
    }

    // Default gossip port: gRPC port + 10000.
    if (gossip_port == 0) gossip_port = grpc_port + 10000;

    // Start gRPC server.
    ReplicaServer server(id, grpc_port, token_delay_ms, max_capacity);
    server.set_model_version(model_version);
    server.set_error_rate(error_rate);
    server.set_reject_all(reject_all);
    g_server = &server;
    server.Start();

    // Start gossip protocol.
    std::string my_gossip_addr = "127.0.0.1:" + std::to_string(gossip_port);
    SwimProtocol swim(id, my_gossip_addr, gossip_port);
    swim.SetMyLoad(0, max_capacity, model_version);

    auto seed_addresses = ParseSeeds(seeds_str);
    if (!seed_addresses.empty()) {
        swim.JoinCluster(seed_addresses);
    }

    g_swim = &swim;
    swim.Start();

    // Periodically update gossip with current load metadata.
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    std::cout << "Press Ctrl+C to stop." << std::endl;

    // Main loop: update load info in gossip every 500ms.
    while (swim.is_running()) {
        swim.SetMyLoad(server.active_requests(), max_capacity, model_version);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    server.Stop();
    return 0;
}
