// Cost of the telemetry metrics recorded on the event loop thread, exported through DogStatsD to a local Unix socket
// drained by another thread (as the Datadog Agent would):
//   * BM_DogStatsDDatagramPerMetric - one metric formatted and sent in its own datagram (the behavior before batching).
//   * BM_DogStatsDBufferedMetric    - one metric formatted into the batch buffer, sent once 8 KiB are buffered.
//   * BM_TelemetryCounterAdd        - TelemetryContext::counterAdd, summed in memory, flushed every kFlushPeriod calls.
//   * BM_Http2RequestMetrics        - the metrics of an HTTP/2 request (9 counters, 10 histograms): one datagram each
//                                     (arg 0, as before batching), or counters summed, histograms buffered and a flush
//                                     every kFlushPeriod requests (arg 1, as the maintenance tick does).

#include <benchmark/benchmark.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

#include "aeronet/dogstatsd.hpp"
#include "aeronet/memory-utils-sv.hpp"
#include "aeronet/metric-label.hpp"
#include "aeronet/telemetry-config.hpp"
#include "aeronet/tracing/tracer.hpp"

namespace aeronet {
namespace {

constexpr int kFlushPeriod = 64;

// Bound Unix datagram socket, whose datagrams a thread receives and discards.
class DrainedSink {
 public:
  DrainedSink() : _path("/tmp/aeronet-telemetry-bench-" + std::to_string(::getpid()) + ".sock") {
    ::unlink(_path.c_str());
    _fd = ::socket(AF_UNIX, SOCK_DGRAM, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    *Append(_path, addr.sun_path) = '\0';
    if (_fd == -1 || ::bind(_fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
      throw std::runtime_error("unable to bind the DogStatsD sink socket");
    }
    timeval recvTimeout{0, 10000};
    ::setsockopt(_fd, SOL_SOCKET, SO_RCVTIMEO, &recvTimeout, sizeof(recvTimeout));
    _drainer = std::thread([this] {
      std::array<char, 1 << 16> buf;
      while (!_stop.load(std::memory_order_relaxed)) {
        benchmark::DoNotOptimize(::recv(_fd, buf.data(), buf.size(), 0));
      }
    });
  }

  DrainedSink(const DrainedSink&) = delete;
  DrainedSink(DrainedSink&&) = delete;
  DrainedSink& operator=(const DrainedSink&) = delete;
  DrainedSink& operator=(DrainedSink&&) = delete;

  ~DrainedSink() {
    _stop.store(true, std::memory_order_relaxed);
    _drainer.join();
    ::close(_fd);
    ::unlink(_path.c_str());
  }

  [[nodiscard]] const std::string& path() const noexcept { return _path; }

 private:
  std::string _path;
  int _fd{-1};
  std::atomic<bool> _stop{false};
  std::thread _drainer;
};

TelemetryConfig DogStatsDConfig(const DrainedSink& sink) {
  TelemetryConfig cfg;
  cfg.withDogStatsdSocketPath(sink.path()).withDogStatsdNamespace("svc").enableDogStatsDMetrics(true);
  return cfg;
}

constexpr MetricLabel kFrameLabels[]{{"direction", "sent"}, {"frame.type", "headers"}};
constexpr MetricLabel kDirectionLabels[]{{"direction", "received"}};
constexpr MetricLabel kRequestLabels[]{{"http.request.method", "GET"}, {"http.response.status_code", "200"}};

void BM_DogStatsDDatagramPerMetric(benchmark::State& state) {
  DrainedSink sink;
  DogStatsD client(sink.path(), "svc");
  for ([[maybe_unused]] auto iter : state) {
    client.increment("aeronet.http2.frames", 1, {}, kFrameLabels);
    client.flush();
  }
}
BENCHMARK(BM_DogStatsDDatagramPerMetric);

void BM_DogStatsDBufferedMetric(benchmark::State& state) {
  DrainedSink sink;
  DogStatsD client(sink.path(), "svc");
  for ([[maybe_unused]] auto iter : state) {
    client.increment("aeronet.http2.frames", 1, {}, kFrameLabels);
  }
}
BENCHMARK(BM_DogStatsDBufferedMetric);

void BM_TelemetryCounterAdd(benchmark::State& state) {
  DrainedSink sink;
  const tracing::TelemetryContext telemetry(DogStatsDConfig(sink));
  const MetricLabels labels = state.range(0) == 0 ? MetricLabels{} : MetricLabels{kFrameLabels};
  int nbCalls = 0;
  for ([[maybe_unused]] auto iter : state) {
    telemetry.counterAdd("aeronet.http2.frames", 1, labels);
    if (++nbCalls == kFlushPeriod) {
      nbCalls = 0;
      telemetry.flush();
    }
  }
}
BENCHMARK(BM_TelemetryCounterAdd)->ArgName("labels")->Arg(0)->Arg(2);

// The metrics recorded by Http2Connection and Http2ProtocolHandler for a GET request on an established connection.
template <class Recorder>
void RecordHttp2RequestMetrics(Recorder& recorder) {
  recorder.counter("aeronet.http2.frames", 1, kDirectionLabels);
  recorder.counter("aeronet.http2.frame.payload.bytes", 23, kDirectionLabels);
  recorder.histogram("aeronet.http2.hpack.block.compressed.bytes", 23.0, kDirectionLabels);
  recorder.histogram("aeronet.http2.hpack.block.header_list.bytes", 184.0, kDirectionLabels);
  recorder.histogram("aeronet.http2.hpack.compression.ratio", 0.125, kDirectionLabels);
  recorder.counter("aeronet.http2.streams.opened", 1, kDirectionLabels);
  recorder.histogram("aeronet.http2.streams.active", 1.0, {});
  recorder.histogram("aeronet.http2.hpack.block.compressed.bytes", 36.0, kFrameLabels);
  recorder.histogram("aeronet.http2.hpack.block.header_list.bytes", 152.0, kFrameLabels);
  recorder.histogram("aeronet.http2.hpack.compression.ratio", 0.236, kFrameLabels);
  recorder.counter("aeronet.http2.frames", 1, kFrameLabels);
  recorder.counter("aeronet.http2.frame.payload.bytes", 36, kFrameLabels);
  recorder.counter("aeronet.http2.frames", 1, kFrameLabels);
  recorder.counter("aeronet.http2.frame.payload.bytes", 1024, kFrameLabels);
  recorder.counter("aeronet.http2.stream.requests", 1, kRequestLabels);
  recorder.histogram("aeronet.http2.stream.duration", 0.362, kRequestLabels);
  recorder.histogram("aeronet.http2.stream.request.body.bytes", 0.0, kRequestLabels);
  recorder.counter("aeronet.http2.streams.closed", 1, kDirectionLabels);
  recorder.histogram("aeronet.http2.streams.active", 0.0, {});
}

struct DatagramPerMetricRecorder {
  void counter(std::string_view name, uint64_t delta, MetricLabels labels) {
    client.increment(name, delta, {}, labels);
    client.flush();
  }

  void histogram(std::string_view name, double value, MetricLabels labels) {
    client.histogram(name, value, {}, labels);
    client.flush();
  }

  DogStatsD& client;
};

struct BatchedRecorder {
  void counter(std::string_view name, uint64_t delta, MetricLabels labels) const {
    telemetry.counterAdd(name, delta, labels);
  }

  void histogram(std::string_view name, double value, MetricLabels labels) const {
    telemetry.histogram(name, value, labels);
  }

  const tracing::TelemetryContext& telemetry;
};

void BM_Http2RequestMetrics(benchmark::State& state) {
  DrainedSink sink;
  if (state.range(0) == 0) {
    DogStatsD client(sink.path(), "svc");
    DatagramPerMetricRecorder recorder{client};
    for ([[maybe_unused]] auto iter : state) {
      RecordHttp2RequestMetrics(recorder);
    }
  } else {
    const tracing::TelemetryContext telemetry(DogStatsDConfig(sink));
    BatchedRecorder recorder{telemetry};
    int nbRequests = 0;
    for ([[maybe_unused]] auto iter : state) {
      RecordHttp2RequestMetrics(recorder);
      if (++nbRequests == kFlushPeriod) {
        nbRequests = 0;
        telemetry.flush();
      }
    }
  }
}
BENCHMARK(BM_Http2RequestMetrics)->ArgName("batched")->Arg(0)->Arg(1);

}  // namespace
}  // namespace aeronet

BENCHMARK_MAIN();
