// TestCluster implementation: in-process test infrastructure.
//
// All components (replicas + gateway) run in the same process but use real
// localhost networking (UDP for gossip, TCP for gRPC). This gives us
// realistic behavior for the distributed protocols while keeping test setup
// fast and deterministic.

#include "tests/test_cluster.h"

#include <chrono>
#include <thread>

#include "common/log.h"
#include "tests/test_port_allocator.h"

namespace llmgateway {

using namespace llmgateway::gossip;

TestCluster::TestCluster() = default;

TestCluster::~TestCluster() {
    StopAll();
}

SwimConfig TestCluster::TestSwimConfig() const {
    SwimConfig c;
    c.protocol_period_ms = 200;
    c.ping_timeout_ms = 100;
    c.suspect_timeout_ms = 1000;
    c.indirect_ping_count = 2;
    return c;
}

std::vector<std::string> TestCluster::GetAliveSeeds(
    const std::string& exclude_id) const {
    // Note: caller may already hold mutex_. This is called from AddReplica
    // which holds the lock, so we don't re-lock here.
    std::vector<std::string> seeds;
    for (const auto& [id, node] : replicas_) {
        if (id == exclude_id) continue;
        if (!node->swim || !node->swim->is_running()) continue;
        seeds.push_back("127.0.0.1:" + std::to_string(node->gossip_port));
    }
    if (gateway_ && gateway_->swim && gateway_->swim->is_running()) {
        seeds.push_back("127.0.0.1:" + std::to_string(gateway_->gossip_port));
    }
    return seeds;
}

// ---------------------------------------------------------------------------
// AddReplica: spawn a new in-process replica.
//
// Allocates two sequential ports so the gateway's address-derivation
// convention (grpc_port = gossip_port - 10000) doesn't apply to us — we
// pass an explicit gRPC address via ReplicaRegistry::SetGrpcAddress after
// the fact instead.
// ---------------------------------------------------------------------------
void TestCluster::AddReplica(const std::string& id,
                             const ReplicaConfig& config) {
    std::lock_guard lock(mutex_);

    auto node = std::make_unique<ReplicaNode>();
    node->id = id;
    node->grpc_port = TestPortAllocator::Allocate();
    node->gossip_port = node->grpc_port + 10000;  // convention for gateway derivation
    node->config = config;

    // Gossip protocol — starts first so it's ready to receive pings.
    std::string gossip_addr = "127.0.0.1:" + std::to_string(node->gossip_port);
    node->swim = std::make_unique<SwimProtocol>(
        id, gossip_addr, node->gossip_port, TestSwimConfig());
    node->swim->SetMyLoad(0, config.max_capacity, config.model_version);

    // Seed from existing replicas + gateway (if already started).
    auto seeds = GetAliveSeeds(id);
    if (!seeds.empty()) {
        node->swim->JoinCluster(seeds);
    }
    node->swim->Start();

    // gRPC server for inference.
    node->server = std::make_unique<ReplicaServer>(
        id, node->grpc_port, config.token_delay_ms, config.max_capacity);
    node->server->set_model_version(config.model_version);
    node->server->set_error_rate(config.error_rate);
    node->server->set_reject_all(config.reject_all);
    node->server->Start();

    LOG_INFO("cluster", "added replica %s (grpc=%d, gossip=%d)",
             id.c_str(), node->grpc_port, node->gossip_port);

    replicas_[id] = std::move(node);
}

// ---------------------------------------------------------------------------
// StartGateway: build the gateway components and start serving.
//
// Components are created in dependency order:
//   1. Registry (bottom) — no dependencies
//   2. LoadBalancer — depends on registry
//   3. CircuitBreakerManager — independent
//   4. RequestQueue — independent
//   5. SwimProtocol — seeds from replicas, sets callback into registry
//   6. GatewayServer — depends on all of the above
//   7. RollingUpdater — depends on registry + lb
// ---------------------------------------------------------------------------
void TestCluster::StartGateway() {
    std::lock_guard lock(mutex_);

    if (gateway_) {
        LOG_WARN("cluster", "gateway already started");
        return;
    }

    gateway_ = std::make_unique<GatewayNode>();
    gateway_->grpc_port = TestPortAllocator::Allocate();
    gateway_->gossip_port = gateway_->grpc_port + 10000;

    gateway_->registry = std::make_unique<ReplicaRegistry>();
    gateway_->lb = std::make_unique<LoadBalancer>(*gateway_->registry);
    gateway_->cb_manager = std::make_unique<CircuitBreakerManager>();
    gateway_->queue = std::make_unique<RequestQueue>(100);

    // Gossip for the gateway (as a non-replica member).
    std::string gossip_addr = "127.0.0.1:" + std::to_string(gateway_->gossip_port);
    gateway_->swim = std::make_unique<SwimProtocol>(
        "gateway-test", gossip_addr, gateway_->gossip_port, TestSwimConfig());

    // Wire membership changes into the registry + rebuild ring.
    // Captured objects live as long as TestCluster does, so raw pointers are safe.
    //
    // Gate the ring rebuild on state change — the callback fires for every
    // load-metadata change too (a gossip round every 200ms), and rebuilding
    // 150 virtual nodes per replica on each of those blocks the gossip
    // receiver thread enough to cause spurious ping timeouts.
    ReplicaRegistry* registry_ptr = gateway_->registry.get();
    LoadBalancer* lb_ptr = gateway_->lb.get();
    SwimProtocol* swim_ptr = gateway_->swim.get();
    gateway_->swim->SetMembershipCallback(
        [registry_ptr, lb_ptr, swim_ptr](const std::string& member_id,
                                         MemberState old_state,
                                         MemberState new_state) {
            auto info = swim_ptr->membership_list().GetMember(member_id);
            if (info) {
                MembershipUpdate update;
                update.set_member_id(member_id);
                update.set_address(info->address);
                update.set_state(info->state);
                update.set_incarnation(info->incarnation);
                update.set_model_version(info->model_version);
                update.set_active_requests(info->active_requests);
                update.set_max_capacity(info->max_capacity);
                registry_ptr->UpdateFromGossip(member_id, update);
            }
            if (old_state != new_state) {
                lb_ptr->RebuildRing();
            }
        });

    // Seed from all currently-running replicas.
    auto seeds = GetAliveSeeds();
    if (!seeds.empty()) {
        gateway_->swim->JoinCluster(seeds);
    }
    gateway_->swim->Start();

    gateway_->server = std::make_unique<GatewayServer>(
        gateway_->grpc_port, *gateway_->registry, *gateway_->lb,
        *gateway_->cb_manager, *gateway_->queue);
    gateway_->server->Start();

    gateway_->updater = std::make_unique<RollingUpdater>(
        *gateway_->registry, *gateway_->lb);

    LOG_INFO("cluster", "started gateway (grpc=%d, gossip=%d)",
             gateway_->grpc_port, gateway_->gossip_port);
}

// ---------------------------------------------------------------------------
// WaitForConvergence: poll until the gateway sees all added replicas as ALIVE.
//
// Uses the registry's GetAliveReplicas() (filters out seeds/gateway/draining)
// and counts how many match our known replica IDs. This is stricter than
// just "gossip has N members" because it also waits for the gateway-specific
// integration steps (gRPC address derivation, registry update, ring rebuild).
// ---------------------------------------------------------------------------
bool TestCluster::WaitForConvergence(int timeout_ms) {
    if (!gateway_) return false;

    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);

    size_t expected;
    {
        std::lock_guard lock(mutex_);
        expected = replicas_.size();
    }

    while (std::chrono::steady_clock::now() < deadline) {
        auto alive = gateway_->registry->GetAliveReplicas();
        if (alive.size() >= expected) {
            // Also ensure gossip has actually propagated max_capacity
            // (avoids the "max_capacity == 0 treated as unlimited" race).
            bool all_have_capacity = true;
            for (const auto& [id, info] : alive) {
                if (info.max_capacity == 0) {
                    all_have_capacity = false;
                    break;
                }
            }
            if (all_have_capacity) return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    return false;
}

std::string TestCluster::GetGatewayAddress() const {
    std::lock_guard lock(mutex_);
    if (!gateway_) return "";
    return "localhost:" + std::to_string(gateway_->grpc_port);
}

std::vector<std::string> TestCluster::GetReplicaIds() const {
    std::lock_guard lock(mutex_);
    std::vector<std::string> ids;
    ids.reserve(replicas_.size());
    for (const auto& [id, _] : replicas_) {
        ids.push_back(id);
    }
    return ids;
}

ReplicaServer* TestCluster::GetReplica(const std::string& id) {
    std::lock_guard lock(mutex_);
    auto it = replicas_.find(id);
    if (it == replicas_.end() || !it->second->server) return nullptr;
    return it->second->server.get();
}

SwimProtocol* TestCluster::GetReplicaSwim(const std::string& id) {
    std::lock_guard lock(mutex_);
    auto it = replicas_.find(id);
    if (it == replicas_.end() || !it->second->swim) return nullptr;
    return it->second->swim.get();
}

SwimProtocol* TestCluster::GetGatewaySwim() {
    std::lock_guard lock(mutex_);
    return gateway_ ? gateway_->swim.get() : nullptr;
}

ReplicaRegistry* TestCluster::GetRegistry() {
    std::lock_guard lock(mutex_);
    return gateway_ ? gateway_->registry.get() : nullptr;
}

CircuitBreakerManager* TestCluster::GetCircuitBreakerManager() {
    std::lock_guard lock(mutex_);
    return gateway_ ? gateway_->cb_manager.get() : nullptr;
}

RequestQueue* TestCluster::GetRequestQueue() {
    std::lock_guard lock(mutex_);
    return gateway_ ? gateway_->queue.get() : nullptr;
}

LoadBalancer* TestCluster::GetLoadBalancer() {
    std::lock_guard lock(mutex_);
    return gateway_ ? gateway_->lb.get() : nullptr;
}

RollingUpdater* TestCluster::GetRollingUpdater() {
    std::lock_guard lock(mutex_);
    return gateway_ ? gateway_->updater.get() : nullptr;
}

std::optional<MemberInfo> TestCluster::GetGatewayViewOfReplica(
    const std::string& id) {
    std::lock_guard lock(mutex_);
    if (!gateway_) return std::nullopt;
    return gateway_->swim->membership_list().GetMember(id);
}

std::optional<MemberInfo> TestCluster::GetReplicaViewOfMember(
    const std::string& observer_id, const std::string& target_id) {
    std::lock_guard lock(mutex_);
    auto it = replicas_.find(observer_id);
    if (it == replicas_.end() || !it->second->swim) return std::nullopt;
    return it->second->swim->membership_list().GetMember(target_id);
}

// ---------------------------------------------------------------------------
// KillReplica: simulates a crash by force-shutting the replica's gRPC
// server and destroying its objects.
//
// We do NOT send a graceful gossip leave — that would defeat the point of
// testing failure detection, since the gossip network is supposed to notice
// via ping timeouts. Gossip threads are joined via the SwimProtocol
// destructor; the gRPC server is force-shutdown via ReplicaServer::Kill()
// so that any in-flight Generate streams are cancelled immediately rather
// than allowed to finish gracefully.
//
// The replica's ports become free after the destructor runs. A subsequent
// RestartReplica() can reuse the same ports.
// ---------------------------------------------------------------------------
void TestCluster::KillReplica(const std::string& id) {
    std::unique_ptr<ReplicaNode> victim;

    {
        std::lock_guard lock(mutex_);
        auto it = replicas_.find(id);
        if (it == replicas_.end()) return;

        // Transfer ownership out of the map so we can destruct without holding
        // the mutex (destructors may block on thread join).
        victim = std::move(it->second);
        // Keep the entry with a null pointer so RestartReplica can recover
        // the ports from our allocation record.
        it->second = std::make_unique<ReplicaNode>();
        it->second->id = id;
        it->second->grpc_port = victim->grpc_port;
        it->second->gossip_port = victim->gossip_port;
        it->second->config = victim->config;
    }

    // Force-cancel any in-flight gRPC streams before the destructor runs.
    // The default grpc::Server destructor triggers a graceful Shutdown,
    // which would let current Generate calls stream all remaining tokens
    // before the server stops — incompatible with a crash simulation.
    if (victim->server) victim->server->Kill();

    // Destruct outside the lock.
    victim.reset();
    LOG_INFO("cluster", "killed replica %s", id.c_str());
}

// ---------------------------------------------------------------------------
// RestartReplica: start a new replica instance on the same ports.
//
// The gossip protocol handles this correctly because the new instance starts
// with incarnation 0 (fresh state) but the existing cluster members will
// update their view via the first PING the new replica sends. The DEAD entry
// for the old incarnation gets overwritten because the new PING carries an
// ALIVE update about the (same) replica_id.
// ---------------------------------------------------------------------------
void TestCluster::RestartReplica(const std::string& id,
                                 const ReplicaConfig& config) {
    std::lock_guard lock(mutex_);

    auto it = replicas_.find(id);
    if (it == replicas_.end()) {
        LOG_WARN("cluster", "cannot restart unknown replica %s", id.c_str());
        return;
    }

    auto& slot = it->second;
    slot->config = config;

    // Rebuild SWIM + gRPC server on the same ports as before.
    std::string gossip_addr = "127.0.0.1:" + std::to_string(slot->gossip_port);
    slot->swim = std::make_unique<SwimProtocol>(
        id, gossip_addr, slot->gossip_port, TestSwimConfig());
    slot->swim->SetMyLoad(0, config.max_capacity, config.model_version);

    auto seeds = GetAliveSeeds(id);
    if (!seeds.empty()) {
        slot->swim->JoinCluster(seeds);
    }
    slot->swim->Start();

    slot->server = std::make_unique<ReplicaServer>(
        id, slot->grpc_port, config.token_delay_ms, config.max_capacity);
    slot->server->set_model_version(config.model_version);
    slot->server->set_error_rate(config.error_rate);
    slot->server->set_reject_all(config.reject_all);
    slot->server->Start();

    LOG_INFO("cluster", "restarted replica %s with version %s",
             id.c_str(), config.model_version.c_str());
}

// ---------------------------------------------------------------------------
// StopAll: shut down everything in a safe order.
//
// Order matters:
//   1. Stop the gateway server first (no new client requests).
//   2. Stop the gateway's gossip (stops talking about membership).
//   3. Stop each replica's gRPC server + gossip.
// Destructors run at the end of scope and clean up UDP/TCP sockets.
// ---------------------------------------------------------------------------
void TestCluster::StopAll() {
    std::unique_ptr<GatewayNode> local_gateway;
    std::unordered_map<std::string, std::unique_ptr<ReplicaNode>> local_replicas;

    {
        std::lock_guard lock(mutex_);
        local_gateway = std::move(gateway_);
        local_replicas = std::move(replicas_);
    }

    // Gateway first.
    if (local_gateway) {
        if (local_gateway->server) local_gateway->server->Stop();
        if (local_gateway->swim) local_gateway->swim->Stop();
    }

    // Then replicas.
    for (auto& [_, node] : local_replicas) {
        if (node->server) node->server->Stop();
        if (node->swim) node->swim->Stop();
    }

    // Destructors run here as the local unique_ptrs go out of scope.
}

size_t TestCluster::NumReplicas() const {
    std::lock_guard lock(mutex_);
    return replicas_.size();
}

}  // namespace llmgateway
