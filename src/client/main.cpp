#include <iostream>
#include "llmgateway.grpc.pb.h"

int main(int argc, char* argv[]) {
    std::cout << "inference client (stub)" << std::endl;

    llmgateway::InferResponse resp;
    resp.set_token("hello");
    resp.set_is_final(false);
    resp.set_replica_id("replica-1");
    std::cout << "proto ok: token=" << resp.token() << std::endl;

    return 0;
}
