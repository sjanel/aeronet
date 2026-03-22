// Benchmark to measure io_uring event-loop and transport throughput.
// End-to-end server benchmark capturing full io_uring effect (poll, accept, send/recv).
#include <benchmark/benchmark.h>

#include <aeronet/aeronet.hpp>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>

#include "bench_util.hpp"

using namespace std::chrono_literals;

namespace aeronet {

namespace {

constexpr auto kPollInterval = 3ms;

SingleHttpServer& getServer() {
  static SingleHttpServer srv{HttpServerConfig{}.withPollInterval(kPollInterval)};
  return srv;
}

void ensureServerStarted() {
  static std::once_flag flag;
  std::call_once(flag, []() { getServer().start(); });
}

// End-to-end benchmark: measures full request/response roundtrip
// with the server using all io_uring optimizations (poll, accept, send/recv).
class IoUringE2EBench : public benchmark::Fixture {
 protected:
  void SetUp(const benchmark::State& state) override {
    ensureServerStarted();
    auto& srv = getServer();
    payloadSize = static_cast<std::size_t>(state.range(0));
    responsePayload = std::string(payloadSize, 'Z');
    srv.router().setDefault([sv = std::string_view(responsePayload)](const HttpRequest&) { return HttpResponse(sv); });
    std::this_thread::sleep_for(2 * kPollInterval);
    client = bench_util::ClientConnection(srv.port());
  }

  [[nodiscard]] bool sendRequest() const {
    static constexpr std::string_view kReq = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    bench_util::sendAll(client.fd(), kReq);
    auto raw = bench_util::recvWithTimeout(client.fd(), 200ms);
    return !raw.empty() && raw.contains("HTTP/1.1 200");
  }

  std::size_t payloadSize{0};
  std::string responsePayload;
  bench_util::ClientConnection client;
};

}  // namespace

}  // namespace aeronet

using namespace aeronet;

BENCHMARK_DEFINE_F(IoUringE2EBench, RequestResponse)(benchmark::State& state) {
  for ([[maybe_unused]] auto iteration : state) {
    if (!sendRequest()) {
      state.SkipWithError("request failed");
      return;
    }
  }
  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(payloadSize));
}

BENCHMARK_REGISTER_F(IoUringE2EBench, RequestResponse)
    ->Unit(benchmark::kMicrosecond)
    ->Args({64})
    ->Args({1024})
    ->Args({4096})
    ->Args({16384})
    ->Args({65536})
    ->Args({262144});

BENCHMARK_MAIN();
