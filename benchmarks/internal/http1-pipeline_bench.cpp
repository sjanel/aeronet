// Throughput of HTTP/1.1 requests pipelined on a single keep-alive connection: each iteration writes a batch of
// pipelined requests at once, then receives all their responses. The server serves many requests from each read of
// its input buffer, which exercises the consumption of the served requests from it and the batching of their responses.
#include <benchmark/benchmark.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "aeronet/http-request-view.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/single-http-server.hpp"
#include "aeronet/test_util.hpp"

#ifdef AERONET_WINDOWS
#include <winsock2.h>
#else
#include <sys/socket.h>
#endif

namespace aeronet {

namespace {

using namespace std::chrono_literals;

constexpr std::string_view kGet = "GET /pipelined HTTP/1.1\r\nHost: localhost\r\nUser-Agent: bench\r\n\r\n";
constexpr std::string_view kPost =
    "POST /pipelined HTTP/1.1\r\nHost: localhost\r\nContent-Length: 16\r\n\r\n0123456789abcdef";

class PipelineFixture : public benchmark::Fixture {
 protected:
  // Argument 0: the number of pipelined requests of a batch. Argument 1: whether TCP_NODELAY is set (the default
  // configuration does not set it on plaintext HTTP/1.1 connections).
  void SetUp(const benchmark::State& state) override {
    stopFlag.store(false, std::memory_order_relaxed);
    // The single connection serves all the iterations.
    HttpServerConfig config =
        HttpServerConfig{}.withPort(0).withMaxRequestsPerConnection(std::numeric_limits<uint32_t>::max());
    if (state.range(1) != 0) {
      config.withTcpNoDelay();
    }
    server = std::make_unique<SingleHttpServer>(std::move(config));
    server->router().setDefault([](const HttpRequestView&) { return HttpResponse("OK"); });
    loopThread =
        std::jthread([this] { server->runUntil([this] { return stopFlag.load(std::memory_order_relaxed); }); });
    client = std::make_unique<test::ClientConnection>(server->port());
    test::setRecvTimeout(client->fd(), 5s);
  }

  void TearDown(const benchmark::State& /*state*/) override {
    client.reset();
    stopFlag.store(true, std::memory_order_relaxed);
    if (loopThread.joinable()) {
      loopThread.join();
    }
    server.reset();
  }

  // Receives exactly nbBytes (the responses all have the same size). Returns false on error or timeout.
  [[nodiscard]] bool recvExactly(std::size_t nbBytes) {
    while (nbBytes != 0) {
      const std::size_t maxBytes = recvBuffer.size() < nbBytes ? recvBuffer.size() : nbBytes;
#ifdef AERONET_WINDOWS
      const auto received = ::recv(client->fd(), recvBuffer.data(), static_cast<int>(maxBytes), 0);
#else
      const auto received = ::recv(client->fd(), recvBuffer.data(), maxBytes, 0);
#endif
      if (received <= 0) {
        return false;
      }
      nbBytes -= static_cast<std::size_t>(received);
    }
    return true;
  }

  void run(benchmark::State& state, std::string_view request) {
    const auto nbRequests = static_cast<std::size_t>(state.range(0));
    std::string batch;
    batch.reserve(nbRequests * request.size());
    for (std::size_t requestPos = 0; requestPos < nbRequests; ++requestPos) {
      batch.append(request);
    }
    recvBuffer.resize(std::size_t{1} << 16U);

    // The size of a response, constant (fixed width Date header): measured on a first request.
    test::sendAll(client->fd(), request);
    const std::size_t responseSize = test::recvWithTimeout(client->fd(), 2s).size();
    if (responseSize == 0) {
      state.SkipWithError("no response");
      return;
    }

    for ([[maybe_unused]] auto iter : state) {
      test::sendAll(client->fd(), batch, 5s);
      if (!recvExactly(nbRequests * responseSize)) {
        state.SkipWithError("responses not received");
        break;
      }
    }
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations() * nbRequests));
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations() * batch.size()));
  }

  std::unique_ptr<SingleHttpServer> server;
  std::atomic<bool> stopFlag{false};
  std::jthread loopThread;
  std::unique_ptr<test::ClientConnection> client;
  std::string recvBuffer;
};

}  // namespace

BENCHMARK_DEFINE_F(PipelineFixture, PipelinedGet)(benchmark::State& state) { run(state, kGet); }
BENCHMARK_REGISTER_F(PipelineFixture, PipelinedGet)
    ->ArgsProduct({{1, 8, 64, 512, 4096}, {0, 1}})
    ->ArgNames({"batch", "nodelay"})
    ->UseRealTime();

BENCHMARK_DEFINE_F(PipelineFixture, PipelinedPostWithBody)(benchmark::State& state) { run(state, kPost); }
BENCHMARK_REGISTER_F(PipelineFixture, PipelinedPostWithBody)
    ->ArgsProduct({{1, 8, 64, 512, 4096}, {1}})
    ->ArgNames({"batch", "nodelay"})
    ->UseRealTime();

}  // namespace aeronet

BENCHMARK_MAIN();
