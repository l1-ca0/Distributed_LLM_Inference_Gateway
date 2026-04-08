#include "gossip/udp_transport.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <iostream>
#include <stdexcept>

namespace llmgateway::gossip {

// Max UDP datagram size for gossip messages.
// SWIM messages with piggybacked updates should fit well within this.
static constexpr size_t kMaxDatagram = 4096;

Address ParseAddress(const std::string& addr_str) {
    auto colon = addr_str.rfind(':');
    if (colon == std::string::npos) {
        throw std::invalid_argument("invalid address (expected host:port): " + addr_str);
    }
    return {addr_str.substr(0, colon),
            static_cast<uint16_t>(std::stoi(addr_str.substr(colon + 1)))};
}

UdpTransport::UdpTransport(uint16_t bind_port) : port_(bind_port) {
    sock_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_fd_ < 0) {
        throw std::runtime_error("failed to create UDP socket: " +
                                 std::string(strerror(errno)));
    }

    // Allow address reuse (useful when restarting quickly)
    int opt = 1;
    setsockopt(sock_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(bind_port);

    if (bind(sock_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(sock_fd_);
        throw std::runtime_error("failed to bind UDP socket to port " +
                                 std::to_string(bind_port) + ": " +
                                 std::string(strerror(errno)));
    }
}

UdpTransport::~UdpTransport() {
    if (sock_fd_ >= 0) {
        close(sock_fd_);
    }
}

bool UdpTransport::Send(const Address& dest, const GossipMessage& message) {
    std::string data;
    if (!message.SerializeToString(&data)) {
        std::cerr << "[udp] failed to serialize message" << std::endl;
        return false;
    }

    if (data.size() > kMaxDatagram) {
        std::cerr << "[udp] message too large: " << data.size() << " bytes" << std::endl;
        return false;
    }

    // Resolve hostname
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    std::string port_str = std::to_string(dest.port);

    int err = getaddrinfo(dest.host.c_str(), port_str.c_str(), &hints, &res);
    if (err != 0 || res == nullptr) {
        std::cerr << "[udp] failed to resolve " << dest.ToString() << ": "
                  << gai_strerror(err) << std::endl;
        return false;
    }

    ssize_t sent = sendto(sock_fd_, data.data(), data.size(), 0,
                          res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);

    if (sent < 0) {
        std::cerr << "[udp] sendto failed: " << strerror(errno) << std::endl;
        return false;
    }

    return true;
}

std::optional<std::pair<GossipMessage, Address>> UdpTransport::Receive(int timeout_ms) {
    struct pollfd pfd{};
    pfd.fd = sock_fd_;
    pfd.events = POLLIN;

    int ret = poll(&pfd, 1, timeout_ms);
    if (ret <= 0) {
        // Timeout or error
        return std::nullopt;
    }

    char buf[kMaxDatagram];
    struct sockaddr_in sender_addr{};
    socklen_t sender_len = sizeof(sender_addr);

    ssize_t received = recvfrom(sock_fd_, buf, sizeof(buf), 0,
                                reinterpret_cast<struct sockaddr*>(&sender_addr),
                                &sender_len);
    if (received <= 0) {
        return std::nullopt;
    }

    GossipMessage message;
    if (!message.ParseFromArray(buf, static_cast<int>(received))) {
        std::cerr << "[udp] failed to parse received message (" << received
                  << " bytes)" << std::endl;
        return std::nullopt;
    }

    // Extract sender address
    char host_buf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &sender_addr.sin_addr, host_buf, sizeof(host_buf));
    Address sender{host_buf, ntohs(sender_addr.sin_port)};

    return std::make_pair(std::move(message), std::move(sender));
}

}  // namespace llmgateway::gossip
