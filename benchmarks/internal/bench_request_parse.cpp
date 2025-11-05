// IWYU pass: ensure each used symbol's header is directly included.
// POSIX / system headers are pulled indirectly by test utilities for sockets.
#include <benchmark/benchmark.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

// Directly include <chrono> already; retain literal operator usage.
using namespace std::chrono_literals;  // NOLINT(misc-unused-using-decls)

#include "aeronet/http-constants.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "bench_util.hpp"  // bench_util::ClientConnection / send helpers

// Self-contained minimal roundtrip benchmark (loopback). Avoids depending on test utilities
// so the benchmarks module can stay decoupled from test headers.

namespace {
class MinimalServerFixture : public benchmark::Fixture {
 protected:
  void SetUp(const benchmark::State& state [[maybe_unused]]) override {
    stopFlag.store(false, std::memory_order_relaxed);
    server = std::make_unique<aeronet::HttpServer>(aeronet::HttpServerConfig{}.withPort(0));
    server->router().setDefault([](const aeronet::HttpRequest&) {
      aeronet::HttpResponse resp;
      resp.body("OK");
      return resp;
    });
    loopThread =
        std::jthread([this] { server->runUntil([this] { return stopFlag.load(std::memory_order_relaxed); }); });
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  void TearDown(const benchmark::State& state [[maybe_unused]]) override {
    stopFlag.store(true, std::memory_order_relaxed);
    if (loopThread.joinable()) {
      loopThread.join();
    }
    server.reset();
  }
  std::unique_ptr<aeronet::HttpServer> server;
  std::atomic<bool> stopFlag{false};
  std::jthread loopThread;
};

bool sendGet(uint16_t port) {
  bench_util::ClientConnection client(port);
  static constexpr std::string_view kReq = "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
  bench_util::sendAll(client.fd(), kReq);
  auto raw = bench_util::recvWithTimeout(client.fd(), 200ms);
  return raw.contains(aeronet::http::DoubleCRLF);
}

BENCHMARK_F(MinimalServerFixture, GET_RoundTrip)(benchmark::State& state) {
  auto portNumber = server->port();
  for ([[maybe_unused]] auto iter : state) {
    if (!sendGet(portNumber)) {
      state.SkipWithError("roundtrip failed");
      break;
    }
  }
  state.counters["port"] = static_cast<double>(portNumber);
}
}  // namespace

BENCHMARK_MAIN();
