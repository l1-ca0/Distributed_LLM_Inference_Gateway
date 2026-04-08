# Distributed LLM Inference Gateway

A high-performance inference gateway in C++17 that routes client requests to a cluster of LLM serving replicas with load balancing, fault tolerance, and streaming token delivery. Replicas use a SWIM gossip protocol for decentralized membership and failure detection.

## Architecture

```
              Clients
                 |  gRPC streaming
                 v
  +-------------------------------+
  |      Inference Gateway        |
  |  - Load Balancer              |
  |  - Request Queue              |
  |  - Membership Subscriber  <.........  gossip (UDP)
  +------+-------+-------+-------+              :
         |       |       |  gRPC streaming      :
         v       v       v                      :
      Rep 1 <-> Rep 2 <-> Rep 3  ...............:
         \________|________/
           gossip (UDP, full mesh)
```

**Two communication layers:**
- **gRPC (TCP):** Client-gateway and gateway-replica inference traffic with server-side streaming
- **UDP:** SWIM gossip protocol for peer-to-peer failure detection and membership management

## Features

- **Gossip-based failure detection** -- SWIM protocol with indirect probing, suspicion mechanism, and incarnation number refutation. No centralized health monitor.
- **KV-cache-aware load balancing with consistent hashing** -- Prompt-prefix affinity routing via a consistent hash ring. When a replica joins or leaves, only ~1/N of mappings are disrupted (preserving KV cache hit rates). Falls back to weighted least-connections when the preferred replica is at capacity.
- **Circuit breaker** -- Detects degraded replicas (high error rate or slow responses) as a complement to gossip (which detects crashes). Automatically stops routing to unhealthy replicas and periodically probes for recovery.
- **Token streaming** -- Gateway proxies tokens from replica to client as they are generated, supporting many concurrent streaming sessions.
- **Mid-stream failover + request hedging** -- If a replica dies mid-stream, the gateway transparently re-routes to another replica. For latency-sensitive requests, speculative execution sends the request to two replicas and streams the faster response (inspired by Google's "The Tail at Scale").
- **Backpressure** -- FIFO request queue with configurable per-replica concurrency limits. Overload returns an error rather than overwhelming replicas.
- **Rolling updates** -- Drain a replica (finish in-flight requests, stop new ones), restart with a new model version, re-join via gossip. Zero dropped requests.

## Relation to Production Systems

The gateway + replicas + streaming + load balancing + failover pattern maps closely to real-world LLM serving infrastructure:

| This Project | Production Equivalent |
|---|---|
| Gateway / Router | vLLM router, TGI router, Kubernetes Ingress |
| Replicas | GPU pods running vLLM / TGI / TensorRT-LLM |
| Health monitoring | Kubernetes liveness probes, Envoy health checks |
| Load balancing | Least-connections or KV-cache-aware routing |
| Backpressure / queuing | Request queues in vLLM, continuous batching |
| Token streaming | SSE or gRPC streaming |

Production LLM serving *requires* multiple replicas not for redundancy but for capacity -- a single GPU can only handle a handful of concurrent requests. Fault tolerance comes as a natural consequence.

**Key simplifications in this project compared to production:**
- No continuous batching (real servers like vLLM dynamically batch multiple requests on the same GPU)
- No prefill/decode disaggregation (production systems may separate prompt processing from token generation onto different hardware)
- Gossip-based health monitoring is a deliberate upgrade over centralized approaches -- production LLM serving typically relies on Kubernetes probes or Envoy health checks (single point of monitoring), while this project uses a SWIM gossip protocol that is fully decentralized, tolerates monitor failure, and scales better with cluster size. Gossip protocols are battle-tested in systems like Cassandra, Consul, and Serf.

## Tech Stack

| Component | Technology |
|-----------|-----------|
| Language | C++17 |
| RPC | gRPC + Protobuf |
| Gossip transport | Raw UDP sockets + Protobuf |
| Build | CMake + Makefile |
| Documentation | Doxygen |

LLM backends are simulated -- each replica receives a prompt, waits a configurable delay per token, and streams back generated tokens. No ML frameworks or GPU required.

## Building

Prerequisites: CMake (>= 3.16), a C++17 compiler, and `git`. No other dependencies are required -- gRPC and Protobuf are automatically downloaded and built from source.

```bash
make all    # builds all binaries and runs the test suite
```

If you already have gRPC and Protobuf installed (e.g., via `brew install grpc protobuf`), you can speed up the build:

```bash
make all USE_SYSTEM_GRPC=ON
```

