#include <iostream>
#include "llmgateway.grpc.pb.h"
#include "gossip.pb.h"

int main(int argc, char* argv[]) {
    std::cout << "replica server (stub)" << std::endl;

    // Verify proto compilation by instantiating a message
    llmgateway::GenerateRequest req;
    req.set_request_id("test");
    std::cout << "proto ok: request_id=" << req.request_id() << std::endl;

    llmgateway::gossip::GossipMessage gossip_msg;
    gossip_msg.set_sender_id("replica-1");
    std::cout << "gossip proto ok: sender_id=" << gossip_msg.sender_id() << std::endl;

    return 0;
}
