#include "gateway/gateway_server.h"

#include <chrono>
#include <future>
#include <thread>
#include <unordered_set>

#include "common/log.h"

namespace llmgateway {

GatewayServer::GatewayServer(int port, ReplicaRegistry& registry,
                             LoadBalancer& lb, CircuitBreakerManager& cb_manager,
                             RequestQueue& queue)
    : port_(port), registry_(registry), lb_(lb), cb_manager_(cb_manager),
      queue_(queue) {}

void GatewayServer::Start() {
    std::string server_address = "0.0.0.0:" + std::to_string(port_);

    grpc::ServerBuilder builder;
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(this);

    server_ = builder.BuildAndStart();
    LOG_INFO("gateway", "listening on %s", server_address.c_str());
}

void GatewayServer::Stop() {
    if (server_) {
        server_->Shutdown();
        LOG_INFO("gateway", "stopped");
    }
}

// ---------------------------------------------------------------------------
// Infer: the main request handler. This is the hot path for every client
// inference call and implements the mid-stream failover strategy.
//
// Retry loop design:
//   The loop runs up to max_retries times. Each iteration:
//     1. Check if the client cancelled (e.g., timeout, user abort).
//     2. Ask the LoadBalancer for a replica, passing an exclude set of
//        replicas already known to be unusable for this request.
//     3. Check the circuit breaker. If the selected replica's circuit is
//        OPEN, add it to the exclude set so the next iteration picks a
//        different replica.
//     4. Increment the active-request counter (for load-balancer awareness),
//        then stream tokens from the replica.
//     5. On success (streamed >= 0): accumulate tokens_sent, record a
//        circuit-breaker success. If all tokens are delivered, return OK.
//     6. On replica failure (streamed == -1): record a circuit-breaker
//        failure, add the replica to the exclude set, and retry.
//        tokens_sent is NOT incremented, so the next replica picks up
//        from where we left off via tokens_already_generated.
//
// The exclude set is the key to robust retries: consistent-hashing selection
// is otherwise deterministic for a given prompt, so without exclusions the
// retry loop would keep picking the same broken replica every iteration.
//
// Partial delivery: if some tokens were sent before all retries exhausted,
// return OK rather than an error. The client already received partial
// output on the stream, so an error status would be confusing. The client
// can detect incompleteness by checking whether is_final was ever true.
//
// RESOURCE_EXHAUSTED: returned when no replica has capacity at all (before
// any retry). This signals the client to back off or queue locally.
// ---------------------------------------------------------------------------
grpc::Status GatewayServer::Infer(grpc::ServerContext* context,
                                  const InferRequest* request,
                                  grpc::ServerWriter<InferResponse>* writer) {
    // Dispatch to hedged path if requested and there are at least 2 replicas.
    if (request->hedge()) {
        auto alive = registry_.GetAliveReplicas();
        if (alive.size() >= 2) {
            return InferHedged(context, request, writer);
        }
        // Fall through to normal path if not enough replicas for hedging.
    }

    const int max_tokens = request->max_tokens();
    const std::string& prompt = request->prompt();
    int tokens_sent = 0;
    int max_retries = 3;

    // Replicas to skip on subsequent retries for this request: circuits
    // that tripped OPEN or streams that just failed. Consistent-hashing
    // selection is deterministic for a given prompt, so without this set
    // the retry loop would keep picking the same broken replica.
    std::unordered_set<std::string> excluded;

    for (int attempt = 0; attempt < max_retries && tokens_sent < max_tokens; attempt++) {
        if (context->IsCancelled()) {
            return grpc::Status::CANCELLED;
        }

        auto selection = lb_.SelectReplica(prompt, excluded);
        if (!selection) {
            // No replica has capacity — wait in the backpressure queue.
            // The calling thread blocks until NotifyCapacityAvailable() is
            // called (when another request finishes) or the timeout expires.
            if (!queue_.WaitForCapacity()) {
                return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                                    "all replicas at capacity and queue full");
            }
            // Retry selection after being woken.
            selection = lb_.SelectReplica(prompt, excluded);
            if (!selection) {
                // Still no capacity (e.g., replica died while we were queued).
                continue;
            }
        }

        // Circuit breaker check: if the replica is in OPEN state, skip it
        // for the remainder of this request and let the LB pick another.
        if (!cb_manager_.AllowRequest(selection->replica_id)) {
            LOG_WARN("gateway", "circuit open for %s, excluding and retrying",
                     selection->replica_id.c_str());
            excluded.insert(selection->replica_id);
            continue;
        }

        // Track this in-flight request so the load balancer accounts for it.
        registry_.IncrementActive(selection->replica_id);

        int streamed = StreamFromReplica(
            selection->replica_id, selection->grpc_address,
            prompt, max_tokens, tokens_sent, writer, context);

        registry_.DecrementActive(selection->replica_id);

        // Notify the backpressure queue that a slot has freed up.
        // This wakes the next FIFO-waiting request thread (if any).
        queue_.NotifyCapacityAvailable();

        if (streamed < 0) {
            // streamed == -1 signals a replica-side gRPC error. Record the
            // failure so the circuit breaker can track the error rate and
            // potentially open the circuit for this replica. Also exclude it
            // from this request's retry set so we pick a different replica.
            cb_manager_.RecordFailure(selection->replica_id);
            excluded.insert(selection->replica_id);
            LOG_WARN("gateway", "replica %s failed after %d tokens, retrying",
                     selection->replica_id.c_str(), tokens_sent);
            continue;
        }

        tokens_sent += streamed;
        cb_manager_.RecordSuccess(selection->replica_id);

        if (tokens_sent >= max_tokens) {
            return grpc::Status::OK;
        }

        // Partial stream (streamed > 0 but < remaining): the replica ended
        // early. Don't retry the same replica — move on.
        excluded.insert(selection->replica_id);
    }

    if (tokens_sent >= max_tokens) {
        return grpc::Status::OK;
    }

    if (tokens_sent > 0) {
        // Partial delivery: some tokens were already written to the stream.
        // Returning an error here would be misleading since the client
        // already received data. Return OK and let the client detect
        // incompleteness via the absence of is_final=true.
        LOG_WARN("gateway", "partial delivery: %d/%d tokens", tokens_sent, max_tokens);
        return grpc::Status::OK;
    }

    return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                        "all replicas failed after retries");
}

// ---------------------------------------------------------------------------
// InferHedged: speculative execution — race two replicas.
//
// Strategy (inspired by Google's "The Tail at Scale"):
//   1. Select two different replicas via the load balancer.
//   2. Open Generate streams to both simultaneously (in separate threads).
//   3. Whichever replica produces its first token faster becomes the "winner."
//   4. Stream all remaining tokens from the winner to the client.
//   5. Cancel the loser's stream to free its capacity.
//
// This reduces tail latency at the cost of extra replica utilization.
// The winner is determined by first-token latency (time to first Read()),
// not by which thread starts first. Both threads read one token, then
// signal via a shared promise. The main thread picks the winner and
// continues streaming from it.
// ---------------------------------------------------------------------------
grpc::Status GatewayServer::InferHedged(grpc::ServerContext* context,
                                        const InferRequest* request,
                                        grpc::ServerWriter<InferResponse>* writer) {
    const int max_tokens = request->max_tokens();
    const std::string& prompt = request->prompt();

    // Select two different replicas.
    auto sel1 = lb_.SelectReplica(prompt);
    if (!sel1) {
        return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                            "no replica available for hedging");
    }

    // For the second replica, we need a different one. Temporarily increment
    // the first replica's active count so the LB deprioritizes it.
    registry_.IncrementActive(sel1->replica_id);
    auto sel2 = lb_.SelectReplica(prompt + "_hedge");  // different hash to get different replica
    registry_.DecrementActive(sel1->replica_id);

    if (!sel2 || sel2->replica_id == sel1->replica_id) {
        // Only one replica available — fall through to non-hedged path.
        // Re-select since we decremented.
        registry_.IncrementActive(sel1->replica_id);
        int streamed = StreamFromReplica(sel1->replica_id, sel1->grpc_address,
                                         prompt, max_tokens, 0, writer, context);
        registry_.DecrementActive(sel1->replica_id);
        queue_.NotifyCapacityAvailable();
        if (streamed >= 0) {
            cb_manager_.RecordSuccess(sel1->replica_id);
            return grpc::Status::OK;
        }
        cb_manager_.RecordFailure(sel1->replica_id);
        return grpc::Status(grpc::StatusCode::UNAVAILABLE, "replica failed");
    }

    LOG_INFO("gateway", "hedging: racing %s vs %s",
             sel1->replica_id.c_str(), sel2->replica_id.c_str());

    registry_.IncrementActive(sel1->replica_id);
    registry_.IncrementActive(sel2->replica_id);

    // Shared state for the race: each thread reads one token, then signals.
    struct RaceResult {
        std::string replica_id;
        std::string grpc_address;
        GenerateResponse first_token;
        std::unique_ptr<grpc::ClientContext> ctx;
        std::unique_ptr<grpc::ClientReader<GenerateResponse>> reader;
        std::unique_ptr<LLMReplica::Stub> stub;
        bool success = false;
    };

    auto race1 = std::make_shared<RaceResult>();
    auto race2 = std::make_shared<RaceResult>();
    race1->replica_id = sel1->replica_id;
    race1->grpc_address = sel1->grpc_address;
    race2->replica_id = sel2->replica_id;
    race2->grpc_address = sel2->grpc_address;

    // Promise: first thread to read a token sets it.
    auto winner_promise = std::make_shared<std::promise<int>>();  // 1 or 2
    auto winner_future = winner_promise->get_future();
    auto winner_set = std::make_shared<std::atomic<bool>>(false);

    // Lambda to open a stream and read the first token.
    auto race_fn = [&](std::shared_ptr<RaceResult> result, int id,
                       std::shared_ptr<std::promise<int>> promise,
                       std::shared_ptr<std::atomic<bool>> flag) {
        auto channel = grpc::CreateChannel(result->grpc_address,
                                           grpc::InsecureChannelCredentials());
        result->stub = LLMReplica::NewStub(channel);
        result->ctx = std::make_unique<grpc::ClientContext>();

        GenerateRequest gen_req;
        gen_req.set_request_id("hedge-" + std::to_string(id));
        gen_req.set_prompt(prompt);
        gen_req.set_max_tokens(max_tokens);
        gen_req.set_tokens_already_generated(0);

        result->reader = result->stub->Generate(result->ctx.get(), gen_req);

        // Read first token.
        if (result->reader->Read(&result->first_token)) {
            result->success = true;
            // Try to be the winner.
            bool expected = false;
            if (flag->compare_exchange_strong(expected, true)) {
                promise->set_value(id);
            }
        }
    };

    // The race threads capture `prompt` and `max_tokens` by reference
    // from this stack frame, so both must be joined before InferHedged
    // returns. The RAII guards enforce this on every exit path including
    // exceptions; the explicit join() calls below handle the normal path.
    struct ThreadJoiner {
        std::thread& t;
        ~ThreadJoiner() { if (t.joinable()) t.join(); }
    };

    std::thread t1(race_fn, race1, 1, winner_promise, winner_set);
    ThreadJoiner j1{t1};
    std::thread t2(race_fn, race2, 2, winner_promise, winner_set);
    ThreadJoiner j2{t2};

    // Wait for the winner (with timeout).
    auto status_future = winner_future.wait_for(std::chrono::seconds(10));

    int winner_id = 0;
    if (status_future == std::future_status::ready) {
        winner_id = winner_future.get();
    }

    // Wait for both threads to finish their first-token read.
    t1.join();
    t2.join();

    auto winner = (winner_id == 1) ? race1 : race2;
    auto loser = (winner_id == 1) ? race2 : race1;

    if (winner_id == 0 || !winner->success) {
        // Neither replica produced a token.
        if (race1->ctx) race1->ctx->TryCancel();
        if (race2->ctx) race2->ctx->TryCancel();
        registry_.DecrementActive(sel1->replica_id);
        registry_.DecrementActive(sel2->replica_id);
        queue_.NotifyCapacityAvailable();
        queue_.NotifyCapacityAvailable();
        return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                            "both hedged replicas failed");
    }

    // Cancel the loser's stream.
    if (loser->ctx) {
        loser->ctx->TryCancel();
    }
    registry_.DecrementActive(loser->replica_id);
    queue_.NotifyCapacityAvailable();
    cb_manager_.RecordSuccess(winner->replica_id);
    if (loser->success) {
        cb_manager_.RecordSuccess(loser->replica_id);  // loser also produced a token
    }

    LOG_INFO("gateway", "hedge winner: %s", winner->replica_id.c_str());

    // Stream the winner's first token to the client.
    InferResponse resp;
    resp.set_token(winner->first_token.token());
    resp.set_is_final(winner->first_token.is_final());
    resp.set_replica_id(winner->replica_id);
    writer->Write(resp);
    int tokens_sent = 1;

    // Continue streaming remaining tokens from the winner.
    GenerateResponse gen_resp;
    while (winner->reader->Read(&gen_resp)) {
        if (context->IsCancelled()) {
            winner->ctx->TryCancel();
            break;
        }

        InferResponse infer_resp;
        infer_resp.set_token(gen_resp.token());
        infer_resp.set_is_final(gen_resp.is_final());
        infer_resp.set_replica_id(winner->replica_id);

        if (!writer->Write(infer_resp)) {
            winner->ctx->TryCancel();
            break;
        }
        tokens_sent++;
    }

    registry_.DecrementActive(winner->replica_id);
    queue_.NotifyCapacityAvailable();

    return grpc::Status::OK;
}

// ---------------------------------------------------------------------------
// StreamFromReplica: the core streaming proxy logic.
//
// Flow:
//   1. Create a gRPC channel+stub to the replica (one per call; channels
//      are lightweight and cached internally by gRPC's channel pool).
//   2. Build a GenerateRequest with the original prompt, max_tokens, and
//      tokens_already_generated. The replica uses tokens_already_generated
//      to skip tokens that were already sent to the client by a previous
//      replica (mid-stream failover).
//   3. Open a server-streaming RPC (Generate) and read tokens in a loop.
//   4. For each token: check if the client cancelled, then forward the
//      token as an InferResponse on the client-facing stream. If the
//      client disconnected (Write returns false), cancel the replica
//      stream to avoid wasting GPU cycles.
//   5. After the Read loop ends, call Finish() to get the final gRPC
//      status from the replica.
//
// Return value convention:
//   >= 0 : number of tokens successfully forwarded to the client.
//     -1 : replica-side error (the gRPC stream terminated with a non-OK
//          status). The caller (Infer) uses -1 to distinguish "zero
//          tokens streamed successfully" from "stream failed before any
//          token". This distinction drives the circuit-breaker recording:
//          -1 triggers RecordFailure; 0 does not.
// ---------------------------------------------------------------------------
int GatewayServer::StreamFromReplica(const std::string& replica_id,
                                     const std::string& grpc_address,
                                     const std::string& prompt,
                                     int max_tokens,
                                     int tokens_already_generated,
                                     grpc::ServerWriter<InferResponse>* writer,
                                     grpc::ServerContext* client_context) {
    auto channel = grpc::CreateChannel(grpc_address,
                                       grpc::InsecureChannelCredentials());
    auto stub = LLMReplica::NewStub(channel);

    GenerateRequest gen_request;
    gen_request.set_request_id("req-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    gen_request.set_prompt(prompt);
    gen_request.set_max_tokens(max_tokens);
    gen_request.set_tokens_already_generated(tokens_already_generated);

    grpc::ClientContext replica_context;
    auto reader = stub->Generate(&replica_context, gen_request);

    int tokens_streamed = 0;
    GenerateResponse gen_response;

    while (reader->Read(&gen_response)) {
        // Early exit if the downstream client has gone away.
        if (client_context->IsCancelled()) {
            replica_context.TryCancel();
            return tokens_streamed;
        }

        // Translate the replica's GenerateResponse into the client-facing
        // InferResponse, tagging it with the serving replica_id for
        // observability (the client can see which replica served each token).
        InferResponse infer_response;
        infer_response.set_token(gen_response.token());
        infer_response.set_is_final(gen_response.is_final());
        infer_response.set_replica_id(replica_id);

        if (!writer->Write(infer_response)) {
            // Client disconnected mid-stream. Cancel the replica RPC to
            // free up its resources (GPU memory, request slot).
            replica_context.TryCancel();
            return tokens_streamed;
        }

        tokens_streamed++;
    }

    // Finish() retrieves the final status from the replica.
    // A non-OK status means the replica encountered an error (crash,
    // timeout, OOM, etc.). We return -1 to signal failure so the Infer
    // retry loop can attempt failover to another replica.
    grpc::Status status = reader->Finish();
    if (!status.ok()) {
        LOG_WARN("gateway", "stream from %s failed: %s",
                 replica_id.c_str(), status.error_message().c_str());
        return -1;
    }

    return tokens_streamed;
}

}  // namespace llmgateway
