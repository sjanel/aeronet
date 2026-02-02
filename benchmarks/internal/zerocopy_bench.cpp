// Benchmark to compare zerocopy vs regular send performance for large payloads.
// This helps tune the kZeroCopyMinPayloadSize threshold.
#include <benchmark/benchmark.h>

#include <aeronet/aeronet.hpp>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>

#include "bench_util.hpp"

using namespace std::chrono_literals;

namespace {

constexpr auto kPollInterval = 3ms;

aeronet::SingleHttpServer server{aeronet::HttpServerConfig{}.withPollInterval(kPollInterval)};

const int kInitOnce = []() {
  server.start();
  return 0;
}();

// Parameterized server fixture supporting zerocopy mode configuration
class ZerocopyServerFixture : public benchmark::Fixture {
 protected:
  void SetUp(const benchmark::State& state) override {
    (void)kInitOnce;  // ensure server is started
    payloadSize = static_cast<std::size_t>(state.range(0));
    const bool zerocopyEnabled = (state.range(1) == 1);

    server.postConfigUpdate([zerocopyEnabled](aeronet::HttpServerConfig& cfg) {
      cfg.withZerocopyMode(zerocopyEnabled ? aeronet::ZerocopyMode::Enabled : aeronet::ZerocopyMode::Disabled);
    });

    // Create the response payload once
    responsePayload = std::string(payloadSize, 'X');

    server.router().setDefault(
        [sv = std::string_view(responsePayload)](const aeronet::HttpRequest&) { return aeronet::HttpResponse(sv); });
    std::this_thread::sleep_for(2 * kPollInterval);  // allow config to propagate
    client = bench_util::ClientConnection(server.port());
  }

  [[nodiscard]] bool sendRequest() const {
    static constexpr std::string_view kReq = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    bench_util::sendAll(client.fd(), kReq);
    auto raw = bench_util::recvWithTimeout(client.fd(), 50ms);
    return raw.ends_with('X') && raw.size() > responsePayload.size();
  }

  std::string responsePayload;
  std::size_t payloadSize{0};
  bench_util::ClientConnection client;
};

// Benchmark: measure request throughput for different payload sizes with zerocopy disabled/enabled
BENCHMARK_DEFINE_F(ZerocopyServerFixture, LargeResponseRoundtrip)(benchmark::State& state) {
  for ([[maybe_unused]] auto iteration : state) {
    if (!sendRequest()) {
      state.SkipWithError("request failed");
      return;
    }
  }
  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(payloadSize));
  state.SetLabel(state.range(1) == 1 ? "zerocopy" : "regular");
}

// Register benchmarks with different payload sizes and zerocopy modes
// Format: ->Args({payload_size, zerocopy_mode})
// zerocopy_mode: 0 = Disabled, 1 = Opportunistic
BENCHMARK_REGISTER_F(ZerocopyServerFixture, LargeResponseRoundtrip)
    ->Unit(benchmark::kMicrosecond)
    // Small payloads (below threshold - should behave the same)
    // ->Args({4096, 0})  // 4KB, regular
    // ->Args({4096, 1})  // 4KB, zerocopy
    // ->Args({8192, 0})  // 8KB, regular
    // ->Args({8192, 1})  // 8KB, zerocopy
    // At threshold (16KB)
    ->Args({16384, 0})  // 16KB, regular
    ->Args({16384, 1})  // 16KB, zerocopy
    // Above threshold - zerocopy should help
    ->Args({32768, 0})    // 32KB, regular
    ->Args({32768, 1})    // 32KB, zerocopy
    ->Args({65536, 0})    // 64KB, regular
    ->Args({65536, 1})    // 64KB, zerocopy
    ->Args({131072, 0})   // 128KB, regular
    ->Args({131072, 1})   // 128KB, zerocopy
    ->Args({262144, 0})   // 256KB, regular
    ->Args({262144, 1})   // 256KB, zerocopy
    ->Args({524288, 0})   // 512KB, regular
    ->Args({524288, 1})   // 512KB, zerocopy
    ->Args({1048576, 0})  // 1MB, regular
    ->Args({1048576, 1})  // 1MB, zerocopy
    ;
}  // namespace

BENCHMARK_MAIN();
