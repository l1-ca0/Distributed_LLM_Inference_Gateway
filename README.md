# Distributed LLM Inference Gateway

A high-performance inference gateway in C++17 that routes client requests to a cluster of LLM serving replicas. The gateway provides prompt-prefix affinity via consistent hashing, weighted load balancing, fault tolerance with mid-stream failover and request hedging, circuit breaker for degraded replica detection, streaming token delivery, backpressure management, and zero-downtime rolling updates. Replicas participate in a SWIM gossip protocol for decentralized membership and failure detection.

## Documentation

- **Final report** — [`docs/final_report.pdf`](docs/final_report.pdf)
- **Code API docs** — open `docs/doxygen/html/index.html` in a browser. Doxygen-generated class and file reference.

## Architecture

```
                    Clients
                       |  gRPC streaming
                       v
        +---------------------------------+
        |        Inference Gateway        |
        |  - Load Balancer                |
        |  - Circuit Breaker              |
        |  - Request Queue                |
        |  - Rolling Updater              |     gossip (UDP)
        |  - Gossip Member  <--------------------------.
        +----+--------+--------+----+                  |
             |        |        |   gRPC streaming      |
             v        v        v                       |
         +------+  +------+  +------+                  |
         | Rep1 |  | Rep2 |  | Rep3 |  ...             |
         +------+  +------+  +------+                  |
            ^         ^         ^                      |
            |         |         |                      |
            '---------+---------'----------------------'
                  gossip (UDP, full mesh)
```

**Two communication layers:**
- **gRPC (TCP):** client-gateway and gateway-replica inference traffic with server-side streaming
- **UDP:** SWIM gossip protocol for peer-to-peer failure detection and membership management

## Features

- **Gossip-based failure detection** — SWIM with indirect probing, suspicion, and incarnation-based refutation. No central health monitor.
- **Prefix-affinity load balancing** — consistent hashing on the 64-char prompt prefix routes identical prefixes to the same replica. Weighted least-connections takes over when the preferred replica is at capacity.
- **Mid-stream failover + request hedging** — if a replica dies mid-stream, the gateway continues the stream on another replica; for latency-sensitive calls, two replicas race and the faster first token wins.
- **Circuit breaker** — catches "gray" failures where a replica is alive in gossip but failing inference requests; complements SWIM's crash detection.
- **Backpressure** — bounded FIFO queue with ticket-based strict ordering; overload returns `RESOURCE_EXHAUSTED`.
- **Rolling updates** — drain, stop, restart, rejoin one replica at a time; the remaining `N-1` replicas absorb traffic so zero requests drop.

## Tech Stack

| Component          | Technology                  |
|--------------------|-----------------------------|
| Language           | C++17                       |
| RPC                | gRPC + Protobuf             |
| Gossip transport   | Raw UDP sockets + Protobuf  |
| Build              | CMake + Makefile            |
| API docs           | Doxygen                     |

LLM backends are simulated — each replica receives a prompt, waits a configurable delay per token, and streams back generated tokens. No ML frameworks or GPUs required.

## Building and Testing

**Prerequisites:** CMake (≥ 3.16), a C++17 compiler, `make`, `git`. gRPC and Protobuf are auto-downloaded via CMake FetchContent if not installed system-wide.

```bash
make all
```

This builds every binary and runs the full test suite.

### Test output

The test suite has four parts, run in sequence:

| Target                     | Tests                                             |
|----------------------------|---------------------------------------------------|
| `test_driver`              | 9 integration tests across 6 design requirements  |
| `test_gossip_unit`         | 14 SWIM protocol unit tests                       |
| `test_swim_integration`    | 4 multi-node SWIM integration tests               |
| `test_cluster_smoke`       | 4 in-process harness smoke tests                  |

### Individual targets

```bash
make build            # build only
make test             # build + run all tests (same as "all")
make clean            # remove the build/ directory
```

Running individual binaries is also possible after `make build`:

```bash
./build/test_driver             # 9 integration tests 
./build/test_gossip_unit        # 14 SWIM unit tests
./build/test_swim_integration   # 4 multi-node integration tests
./build/test_cluster_smoke      # 4 TestCluster harness smoke tests
```

- `USE_SYSTEM_GRPC=ON` — use an already-installed gRPC (via `pkg-config`) instead of FetchContent. The Makefile auto-detects this; override with `make all USE_SYSTEM_GRPC=OFF` to force the from-source build.
- `CMAKE_BUILD_TYPE=Debug` — build with debug symbols and no optimization.

## Project Structure

```
src/
  gossip/      SWIM protocol (UDP transport, membership list, protocol loop)
  gateway/     Gateway server, load balancer, circuit breaker, queue, rolling updater
  replica/     Simulated LLM replica (gRPC Generate service, fault injection hooks)
  client/      Blocking gRPC client used by tests and the standalone binary
  common/      Shared utilities (logging)
tests/         Integration test suite + in-process TestCluster harness
proto/         gRPC/Protobuf service definitions
docs/          Reports and Doxygen output
```
