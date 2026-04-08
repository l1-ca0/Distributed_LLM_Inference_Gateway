#pragma once

#include <optional>
#include <string>
#include <utility>

#include "gossip.pb.h"

namespace llmgateway::gossip {

// Parsed network address
struct Address {
    std::string host;
    uint16_t port;

    std::string ToString() const { return host + ":" + std::to_string(port); }

    bool operator==(const Address& other) const {
        return host == other.host && port == other.port;
    }
};

// Parse "host:port" string into Address
Address ParseAddress(const std::string& addr_str);

// UDP transport for sending/receiving protobuf-serialized GossipMessages.
// Thread-safe: Send() and Receive() can be called from different threads.
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
