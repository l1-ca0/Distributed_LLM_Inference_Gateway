#include <csignal>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "common/log.h"
#include "gateway/circuit_breaker.h"
#include "gateway/gateway_server.h"
#include "gateway/load_balancer.h"
#include "gateway/replica_registry.h"
#include "gossip/swim.h"

using namespace llmgateway;
using namespace llmgateway::gossip;

static GatewayServer* g_server = nullptr;

void signal_handler(int signal) {
    if (g_server) {
        g_server->Stop();
    }
}

// Parse comma-separated seed addresses.
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
    int grpc_port = 8080;
    int gossip_port = 18080;
    std::string seeds_str;
    std::string gateway_id = "gateway-1";

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) grpc_port = std::stoi(argv[++i]);
        else if (arg == "--gossip-port" && i + 1 < argc) gossip_port = std::stoi(argv[++i]);
        else if (arg == "--seeds" && i + 1 < argc) seeds_str = argv[++i];
        else if (arg == "--id" && i + 1 < argc) gateway_id = argv[++i];
    }

    auto seed_addresses = ParseSeeds(seeds_str);

    // Components
    ReplicaRegistry registry;
    LoadBalancer lb(registry);
    CircuitBreakerManager cb_manager;
    RequestQueue queue(100);  // max 100 queued requests before rejecting

    // Gossip: gateway joins the cluster as a non-replica member.
    std::string my_gossip_addr = "127.0.0.1:" + std::to_string(gossip_port);
    SwimProtocol swim(gateway_id, my_gossip_addr, gossip_port);

    // On membership change → update the replica registry and rebuild the hash ring.
    swim.SetMembershipCallback([&](const std::string& id, MemberState old_s, MemberState new_s) {
        auto info = swim.membership_list().GetMember(id);
        if (info) {
            MembershipUpdate update;
            update.set_member_id(id);
            update.set_address(info->address);
            update.set_state(info->state);
            update.set_incarnation(info->incarnation);
            update.set_model_version(info->model_version);
            update.set_active_requests(info->active_requests);
            update.set_max_capacity(info->max_capacity);
            registry.UpdateFromGossip(id, update);
        }
        lb.RebuildRing();

        const char* state_str = (new_s == ALIVE) ? "ALIVE" :
                                (new_s == SUSPECT) ? "SUSPECT" : "DEAD";
        LOG_INFO("gateway", "membership change: %s → %s", id.c_str(), state_str);
    });

    swim.JoinCluster(seed_addresses);
    swim.Start();

    // Wait for gossip convergence before accepting client requests.
    LOG_INFO("gateway", "waiting for replica discovery via gossip...");
    for (int i = 0; i < 50; i++) {  // up to 5 seconds
        if (registry.GetAliveReplicas().size() > 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    auto alive = registry.GetAliveReplicas();
    LOG_INFO("gateway", "discovered %zu replicas", alive.size());

    // Build initial hash ring.
    lb.RebuildRing();

    // Start gRPC server.
    GatewayServer server(grpc_port, registry, lb, cb_manager, queue);
    g_server = &server;

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    server.Start();

    std::cout << "Press Ctrl+C to stop." << std::endl;
    pause();

    swim.Stop();
    return 0;
}
