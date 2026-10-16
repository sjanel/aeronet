// Benchmark to compare MSG_ZEROCOPY vs regular sends of large responses.
// Client and server run on the same host: the kernel copies the data looped to a local socket anyway (deferred copy),
// so this measures the overhead of MSG_ZEROCOPY (page pinning, completion notifications, retained buffers), not its
// gains. Measuring them, to tune HttpServerConfig::zerocopyMinBytes, needs the client on another host (real NIC).
#include <benchmark/benchmark.h>

#include <aeronet/aeronet-server.hpp>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>

#ifdef AERONET_LINUX
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#endif

#include "bench_util.hpp"

using namespace std::chrono_literals;

namespace aeronet {

namespace {

constexpr auto kPollInterval = 3ms;

aeronet::SingleHttpServer server{aeronet::HttpServerConfig{}.withPollInterval(kPollInterval)};

const int kInitOnce = [] {
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
      // Enabled: Opportunistic would never use MSG_ZEROCOPY for this loopback client. Every payload size is sent with
      // it.
      cfg.withZerocopyMode(zerocopyEnabled ? aeronet::ZerocopyMode::Enabled : aeronet::ZerocopyMode::Disabled);
      cfg.withZerocopyMinBytes(1U);
    });

    // Create the response payload once
    responsePayload = std::string(payloadSize, 'X');

    server.router().setDefault([sv = std::string_view(responsePayload)](const aeronet::HttpRequestView&) {
      return aeronet::HttpResponse(sv);
    });
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

}  // namespace

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
// zerocopy_mode: 0 = Disabled, 1 = Enabled (MSG_ZEROCOPY for every payload)
BENCHMARK_REGISTER_F(ZerocopyServerFixture, LargeResponseRoundtrip)
    ->Unit(benchmark::kMicrosecond)
    // Small payloads: pinning pages costs more than copying them
    // ->Args({4096, 0})  // 4KB, regular
    // ->Args({4096, 1})  // 4KB, zerocopy
    // ->Args({8192, 0})  // 8KB, regular
    // ->Args({8192, 1})  // 8KB, zerocopy
    ->Args({16384, 0})    // 16KB, regular
    ->Args({16384, 1})    // 16KB, zerocopy
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

#ifdef AERONET_LINUX
namespace {

constexpr std::size_t kStreamedBodySize = 8UL << 20U;

// Small enough for the server writes to quickly fill the socket, as for a slow reader, but at least the loopback MSS
// (64 KiB): below it, the sender would wait for window probes (silly window syndrome avoidance).
constexpr int kSlowReaderReceiveBufferSize = 256 * 1024;

// Pause of the slow reader after each read (of at most 64 KiB): it reads at most 64 KiB every 200 us, ~300 MiB/s.
constexpr auto kSlowReaderPause = std::chrono::microseconds{200};

// User + system CPU time consumed so far, in microseconds, by this process (RUSAGE_SELF) or thread (RUSAGE_THREAD).
std::uint64_t CpuTimeUs(int who) {
  rusage usage{};
  // Only fails for an invalid 'who' or address.
  [[maybe_unused]] const int ret = ::getrusage(who, &usage);
  assert(ret == 0);
  const auto micros = [](const timeval& tv) {
    return (static_cast<std::uint64_t>(tv.tv_sec) * 1000000ULL) + static_cast<std::uint64_t>(tv.tv_usec);
  };
  return micros(usage.ru_utime) + micros(usage.ru_stime);
}

// A streaming handler writing a large chunked response to a slow reader (small receive buffer, limited read rate): the
// handler writes all its chunks at once, much faster than the client reads them, so they wait in the connection output
// behind the bytes the socket did not take. Reports the server CPU time per response (process CPU time minus the one
// of this client thread), which includes the copies of this waiting output.
class StreamingSlowReaderFixture : public benchmark::Fixture {
 protected:
  void SetUp(const benchmark::State& state) override {
    (void)kInitOnce;  // ensure server is started
    const auto chunkSize = static_cast<std::size_t>(state.range(0));
    const bool zerocopyEnabled = state.range(1) == 1;

    server.postConfigUpdate([zerocopyEnabled](aeronet::HttpServerConfig& cfg) {
      // Enabled: Opportunistic would never use MSG_ZEROCOPY for this loopback client.
      cfg.withZerocopyMode(zerocopyEnabled ? aeronet::ZerocopyMode::Enabled : aeronet::ZerocopyMode::Disabled);
      cfg.withZerocopyMinBytes(aeronet::HttpServerConfig{}.zerocopyMinBytes);
      // The whole response may wait in the connection output: it must not be refused as an overflow.
      cfg.withMaxOutboundBufferBytes(static_cast<uint32_t>(2 * kStreamedBodySize));
    });

    server.router().setDefault(
        [chunk = std::string(chunkSize, 'X')](const aeronet::HttpRequestView&, aeronet::HttpResponseWriter& writer) {
          for (std::size_t written = 0; written < kStreamedBodySize; written += chunk.size()) {
            if (!writer.writeBody(chunk)) {
              break;
            }
          }
          writer.end();
        });
    std::this_thread::sleep_for(2 * kPollInterval);  // allow config to propagate
    client = bench_util::ClientConnection(server.port(), 500ms, kSlowReaderReceiveBufferSize);
    aeronet::test::setRecvTimeout(client.fd(), 2000ms);
  }

  // Sends a request and receives its whole response, until its last chunk (the body is made of 'X' only: the last
  // chunk is the only "0\r\n\r\n" of the response).
  [[nodiscard]] bool streamResponse() {
    static constexpr std::string_view kReq = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    bench_util::sendAll(client.fd(), kReq);
    std::size_t received = 0;
    tail.clear();
    while (received <= kStreamedBodySize || !tail.ends_with(http::EndChunk)) {
      const auto nb = ::recv(client.fd(), recvBuffer, sizeof(recvBuffer), 0);
      if (nb <= 0) {
        return false;
      }
      received += static_cast<std::size_t>(nb);
      tail.append(recvBuffer, static_cast<std::size_t>(nb));
      tail.erase(0, tail.size() - std::min(tail.size(), http::EndChunk.size()));
      std::this_thread::sleep_for(kSlowReaderPause);
    }
    return true;
  }

  char recvBuffer[1UL << 16U];
  std::string tail;
  bench_util::ClientConnection client;
};

}  // namespace

BENCHMARK_DEFINE_F(StreamingSlowReaderFixture, ChunkedStreamToSlowReader)(benchmark::State& state) {
  std::uint64_t serverCpuUs = 0;
  const auto statsBefore = server.stats();
  for ([[maybe_unused]] auto iteration : state) {
    const auto processStart = CpuTimeUs(RUSAGE_SELF);
    const auto clientStart = CpuTimeUs(RUSAGE_THREAD);
    if (!streamResponse()) {
      state.SkipWithError("streamed response not received");
      return;
    }
    const auto clientCpu = CpuTimeUs(RUSAGE_THREAD) - clientStart;
    serverCpuUs += CpuTimeUs(RUSAGE_SELF) - processStart - clientCpu;
  }
  state.counters["server_cpu_us"] =
      benchmark::Counter(static_cast<double>(serverCpuUs), benchmark::Counter::kAvgIterations);
  // Per response: the flushes of the waiting output, its zerocopy sends, and those refused by the retention limit.
  const auto statsAfter = server.stats();
  const auto perResponse = [](uint64_t before, uint64_t after) {
    return benchmark::Counter(static_cast<double>(after - before), benchmark::Counter::kAvgIterations);
  };
  state.counters["flushes"] = perResponse(statsBefore.flushCycles, statsAfter.flushCycles);
  state.counters["zc_sends"] = perResponse(statsBefore.zerocopySends, statsAfter.zerocopySends);
  state.counters["zc_bytes"] = perResponse(statsBefore.zerocopyBytes, statsAfter.zerocopyBytes);
  state.counters["zc_retain_fallbacks"] =
      perResponse(statsBefore.zerocopyRetainLimitFallbacks, statsAfter.zerocopyRetainLimitFallbacks);
  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(kStreamedBodySize));
  state.SetLabel(state.range(1) == 1 ? "zerocopy" : "regular");
}

// Format: ->Args({chunk_size, zerocopy_mode}), for a response of kStreamedBodySize bytes.
// zerocopy_mode: 0 = Disabled, 1 = Enabled (Opportunistic is Disabled for a loopback client).
BENCHMARK_REGISTER_F(StreamingSlowReaderFixture, ChunkedStreamToSlowReader)
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime()
    ->Args({256, 0})      // small chunks (events), regular
    ->Args({256, 1})      // small chunks (events), zerocopy
    ->Args({4096, 0})     // 4KB chunks, regular
    ->Args({4096, 1})     // 4KB chunks, zerocopy
    ->Args({65536, 0})    // 64KB chunks, regular
    ->Args({65536, 1})    // 64KB chunks, zerocopy
    ->Args({262144, 0})   // 256KB chunks, regular
    ->Args({262144, 1});  // 256KB chunks, zerocopy
#endif

}  // namespace aeronet

BENCHMARK_MAIN();
