#pragma once

// UDP transport layer for the SWIM gossip protocol.
// Sends and receives length-prefixed protobuf datagrams over a single
// bound socket. See swim.h for the protocol that uses this transport.

#include <optional>
#include <string>
#include <utility>

#include "gossip.pb.h"

namespace llmgateway::gossip {

// Parsed network address used throughout the gossip layer.
// Addresses are always "host:port" strings at rest (in protobuf fields,
// config, and seed lists); this struct exists for comparisons and for
// passing host/port to syscalls without re-parsing.
struct Address {
    std::string host;
    uint16_t port;

    std::string ToString() const { return host + ":" + std::to_string(port); }

    bool operator==(const Address& other) const {
        return host == other.host && port == other.port;
    }
};

// Parse a "host:port" string into an Address. Throws std::invalid_argument
// on malformed input (missing colon, non-numeric port, port out of range).
// IPv6 addresses are not supported — hostnames are treated as-is and
// resolved by the OS at send time.
Address ParseAddress(const std::string& addr_str);

// ---------------------------------------------------------------------------
// UdpTransport: thin UDP wrapper used by the SWIM gossip layer.
//
// Owns a single bound UDP socket. Messages are GossipMessage protobufs
// serialized to bytes; datagrams are assumed to fit in a single packet
// (SWIM gossip messages with piggyback are well under the typical 64KB
// IP datagram limit at the cluster sizes this project targets).
//
// Thread-safety: Send() and Receive() may be called concurrently from
// different threads — the OS socket API is thread-safe for this pattern,
// and the SWIM protocol uses exactly one sender thread and one receiver
// thread. Copying and moving are disabled because the socket fd is owned.
//
// Lifetime: the socket is created in the constructor and closed in the
// destructor. Construction throws std::runtime_error if bind() fails
// (e.g., port already in use).
// ---------------------------------------------------------------------------
class UdpTransport {
public:
    explicit UdpTransport(uint16_t bind_port);
    ~UdpTransport();

    // Non-copyable, non-movable (owns a socket)
    UdpTransport(const UdpTransport&) = delete;
    UdpTransport& operator=(const UdpTransport&) = delete;

    // Send a GossipMessage to the given address. Returns true on success.
    bool Send(const Address& dest, const GossipMessage& message);

    // Receive a GossipMessage with a timeout. Returns nullopt on timeout or error.
    std::optional<std::pair<GossipMessage, Address>> Receive(int timeout_ms);

    uint16_t port() const { return port_; }

private:
    int sock_fd_ = -1;
    uint16_t port_;
};

}  // namespace llmgateway::gossip
