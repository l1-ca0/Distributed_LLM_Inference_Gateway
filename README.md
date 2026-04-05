# Distributed LLM Inference Gateway

A high-performance inference gateway in C++17 that routes client requests to a cluster of LLM serving replicas with load balancing, fault tolerance, and streaming token delivery. Replicas use a SWIM gossip protocol for decentralized membership and failure detection.

## Architecture

```
              Clients
                 |  gRPC streaming (TCP)
                 v
  +-------------------------------+
  |      Inference Gateway        |
  |  - Load Balancer              |
  |  - Request Queue              |
  |  - Membership Subscriber  <........  gossip (UDP)
  +------+-------+-------+-------+            :
         |       |       |  gRPC (TCP)        :
         v       v       v                    :
      Rep 1 <-> Rep 2 <-> Rep 3  .............:
              gossip (UDP)
```

**Two communication layers:**
- **gRPC (TCP):** Client-gateway and gateway-replica inference traffic with server-side streaming
- **UDP:** SWIM gossip protocol for peer-to-peer failure detection and membership management

## Features

- **Gossip-based failure detection** -- SWIM protocol with indirect probing, suspicion mechanism, and incarnation number refutation. No centralized health monitor.
- **Weighted least-connections load balancing** -- Routes requests to the least-loaded replica, with gossip-propagated load metadata for accurate routing decisions.
- **Token streaming** -- Gateway proxies tokens from replica to client as they are generated, supporting many concurrent streaming sessions.
- **Mid-stream failover** -- If a replica dies during active token generation, the gateway transparently re-routes to another replica and resumes streaming.
- **Backpressure** -- FIFO request queue with configurable per-replica concurrency limits. Overload returns an error rather than overwhelming replicas.
- **Rolling updates** -- Drain a replica (finish in-flight requests, stop new ones), restart with a new model version, re-join via gossip. Zero dropped requests.

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

```bash
make all    # builds all binaries and runs the test suite
```

