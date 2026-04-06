#include <iostream>
#include "llmgateway.grpc.pb.h"
#include "gossip.pb.h"

int main(int argc, char* argv[]) {
    std::cout << "gateway server (stub)" << std::endl;

    llmgateway::InferRequest req;
    req.set_client_id("client-1");
    req.set_prompt("hello");
    req.set_max_tokens(10);
    std::cout << "proto ok: client_id=" << req.client_id() << std::endl;

    return 0;
}
