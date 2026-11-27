// Cost of the OpenTelemetry span of a request, made as HttpRequestView makes it: created once the head is parsed, with
// the method, target, scheme and host attributes, then the status code set and the span ended once the response is
// finalized. The spans go through TelemetryContext: its span recorder encodes them in OTLP and hands them off in
// batches to its exporter thread, which posts them with the OTLP HTTP client of opentelemetry-cpp to an aeronet server
// on another thread, that accepts every export (as an OpenTelemetry collector would).
//
// The spans are made in rounds of one export batch (512 spans, SpanRecorder::kSpansPerBatch): the recorder hands the
// batch off as the last one ends, and the timer is paused until the collector received it: the export does not overlap
// the timed span making, and no span is dropped. With the trace SDK of opentelemetry-cpp, used before the recorder, a
// thread making nothing but spans outpaced the exporter: its queue of 2048 spans filled up within 10 K spans.
//
// Arguments: `sampled` (sample rate 1 or 0) and `long_values` (target and host shorter than 16 characters, each in the
// inline storage of a std::string, or a target of 26 characters and a host of 20).
//
// Counters, per span:
//   * loop_allocs, loop_bytes - allocations (malloc, calloc and realloc calls) and bytes requested by the thread making
//                               the spans: the event loop thread of a server.
//   * export_allocs           - allocations of the other threads: the exporter thread of the recorder (the request
//                               and its serialization) and the HTTP client. Not the collector server, whose thread is
//                               not counted. The exporter may still be allocating for a batch after the collector
//                               received it: each round counts the end of the previous one.
//   * export_ns               - time from the last span of a round to the collector receiving the batch: an upper
//                               bound of the exporter's time per span (it includes the loopback and the collector).
// The benchmark fails if the collector did not receive every span.
#include <benchmark/benchmark.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>

#include "aeronet/http-request-view.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/router.hpp"
#include "aeronet/single-http-server.hpp"
#include "aeronet/telemetry-config.hpp"
#include "aeronet/tracing/tracer.hpp"

// glibc entry points of its allocator, called by the counting interposers below.
extern "C" void* __libc_malloc(std::size_t size) noexcept;                     // NOLINT(bugprone-reserved-identifier)
extern "C" void* __libc_calloc(std::size_t nmemb, std::size_t size) noexcept;  // NOLINT(bugprone-reserved-identifier)
extern "C" void* __libc_realloc(void* ptr, std::size_t size) noexcept;         // NOLINT(bugprone-reserved-identifier)

namespace aeronet {
namespace {

// The allocations of one thread, written by that thread only, on a cache line of its own: counting adds no contention
// between the threads making and exporting the spans.
struct alignas(64) ThreadAllocations {
  std::atomic<uint64_t> nbAllocations;
  std::atomic<uint64_t> nbBytes;
  std::atomic<bool> excluded;
};

constexpr std::size_t kMaxCountedThreads = 256;

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables): state of the malloc interposers
std::array<ThreadAllocations, kMaxCountedThreads> gThreadAllocations;
// Threads beyond kMaxCountedThreads share this one, with atomic increments.
ThreadAllocations gSharedThreadAllocations;
std::atomic<std::size_t> gNbCountedThreads;
thread_local ThreadAllocations* tThreadAllocations = nullptr;
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

ThreadAllocations& CurrentThreadAllocations() noexcept {
  if (tThreadAllocations == nullptr) {
    const std::size_t pos = gNbCountedThreads.fetch_add(1, std::memory_order_relaxed);
    tThreadAllocations = pos < kMaxCountedThreads ? &gThreadAllocations[pos] : &gSharedThreadAllocations;
  }
  return *tThreadAllocations;
}

void CountAllocation(std::size_t size) noexcept {
  ThreadAllocations& counts = CurrentThreadAllocations();
  if (&counts == &gSharedThreadAllocations) {
    counts.nbAllocations.fetch_add(1, std::memory_order_relaxed);
    counts.nbBytes.fetch_add(size, std::memory_order_relaxed);
  } else {
    counts.nbAllocations.store(counts.nbAllocations.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    counts.nbBytes.store(counts.nbBytes.load(std::memory_order_relaxed) + size, std::memory_order_relaxed);
  }
}

struct AllocationTotals {
  uint64_t loopAllocations;
  uint64_t loopBytes;
  uint64_t otherAllocations;
};

// Allocations of the calling thread, and of all the other threads not excluded.
AllocationTotals CurrentAllocationTotals() noexcept {
  const ThreadAllocations& own = CurrentThreadAllocations();
  AllocationTotals totals{own.nbAllocations.load(std::memory_order_relaxed),
                          own.nbBytes.load(std::memory_order_relaxed), 0};
  const std::size_t nbThreads = std::min(gNbCountedThreads.load(std::memory_order_relaxed), kMaxCountedThreads);
  for (std::size_t pos = 0; pos < nbThreads; ++pos) {
    const ThreadAllocations& counts = gThreadAllocations[pos];
    if (&counts != &own && !counts.excluded.load(std::memory_order_relaxed)) {
      totals.otherAllocations += counts.nbAllocations.load(std::memory_order_relaxed);
    }
  }
  totals.otherAllocations += gSharedThreadAllocations.nbAllocations.load(std::memory_order_relaxed);
  return totals;
}

// An OTLP/HTTP collector accepting every export, served by an aeronet server on a thread of its own whose allocations
// are not counted. It counts the spans it receives.
class SpanCollector {
 public:
  SpanCollector() : _server(HttpServerConfig{}.withPort(0)) {
    _server.router().setDefault([this](const HttpRequestView& req) {
      if (req.path() == "/v1/traces") {
        _nbSpans.fetch_add(CountSpans(req.body()), std::memory_order_relaxed);
      }
      // Both OTLP success responses serialize to nothing.
      return HttpResponse(http::StatusCodeOK);
    });
    // Excluded before the server thread allocates anything.
    _thread = std::jthread([this] {
      CurrentThreadAllocations().excluded.store(true, std::memory_order_relaxed);
      _server.run();
    });
  }

  SpanCollector(const SpanCollector&) = delete;
  SpanCollector(SpanCollector&&) = delete;
  SpanCollector& operator=(const SpanCollector&) = delete;
  SpanCollector& operator=(SpanCollector&&) = delete;

  ~SpanCollector() {
    _server.stop();
    _thread.join();
  }

  [[nodiscard]] std::string tracesEndpoint() const {
    return "http://127.0.0.1:" + std::to_string(_server.port()) + "/v1/traces";
  }

  [[nodiscard]] uint64_t nbSpans() const noexcept { return _nbSpans.load(std::memory_order_relaxed); }

  // Waits until the collector received nbSpans spans in all. Returns false after a second without them.
  [[nodiscard]] bool waitForSpans(uint64_t nbSpans) const {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
    while (this->nbSpans() < nbSpans) {
      if (std::chrono::steady_clock::now() > deadline) {
        return false;
      }
      std::this_thread::yield();
    }
    return true;
  }

 private:
  // The spans of an ExportTraceServiceRequest, counted by their name: field 5 of Span (tag 0x2A), 12 bytes long.
  static uint64_t CountSpans(std::string_view body) noexcept {
    static constexpr std::string_view kSerializedSpanName{
        "\x2A\x0C"
        "http.request"};
    uint64_t nbSpans = 0;
    for (auto pos = body.find(kSerializedSpanName); pos != std::string_view::npos;
         pos = body.find(kSerializedSpanName, pos + kSerializedSpanName.size())) {
      ++nbSpans;
    }
    return nbSpans;
  }

  SingleHttpServer _server;
  std::atomic<uint64_t> _nbSpans{0};
  std::jthread _thread;
};

struct RequestHead {
  std::string_view target;
  std::string_view host;
};

constexpr RequestHead kRequestHeads[]{{"/ping", "localhost:8080"},
                                      {"/api/v1/users/42/orders/17", "api.example.com:8443"}};

// SpanRecorder::kSpansPerBatch: the recorder hands a batch off to its exporter thread as soon as it has that many
// spans.
constexpr uint64_t kSpansPerExport = 512;

constexpr uint64_t kWarmUpRounds = 3;

// The span of an HTTP/1.1 request, as HttpRequestView::initTrySetHead() and HttpRequestView::end() make it.
void RecordRequestSpan(const tracing::TelemetryContext& telemetry, const RequestHead& head) {
  const tracing::SpanPtr span = telemetry.createSpan("http.request");
  if (span) {
    span->setAttribute("http.method", std::string_view{"GET"});
    span->setAttribute("http.target", head.target);
    span->setAttribute("http.scheme", std::string_view{"http"});
    span->setAttribute("http.host", head.host);
    span->setAttribute("http.status_code", int64_t{200});
    span->end();
  }
}

void BM_RequestSpan(benchmark::State& state) {
  const bool sampled = state.range(0) != 0;
  const RequestHead& head = kRequestHeads[static_cast<std::size_t>(state.range(1))];
  SpanCollector collector;
  uint64_t nbSpansEnded = 0;
  {
    TelemetryConfig cfg;
    cfg.otelEnabled = true;
    cfg.withEndpoint(collector.tracesEndpoint()).withServiceName("aeronet-bench").withSampleRate(sampled ? 1.0 : 0.0);
    // Also the delay before a flush() hands a partial batch off: unused here, the benchmark does not flush.
    cfg.exportInterval = std::chrono::milliseconds{100};
    cfg.exportTimeout = std::chrono::milliseconds{99};
    const tracing::TelemetryContext telemetry(cfg);

    // Warm up rounds, not counted: the record of the recorder, its three batch buffers (they rotate between the
    // recorder and its exporter thread, each grows during its first round), the connection of the exporter.
    for (uint64_t round = 0; round < kWarmUpRounds; ++round) {
      for (uint64_t spanPos = 0; spanPos < kSpansPerExport; ++spanPos) {
        RecordRequestSpan(telemetry, head);
      }
      nbSpansEnded += sampled ? kSpansPerExport : 0;
      if (!collector.waitForSpans(nbSpansEnded)) {
        state.SkipWithError("the warm up spans were not exported");
        return;
      }
    }

    const AllocationTotals before = CurrentAllocationTotals();
    AllocationTotals lastExport = before;
    std::chrono::steady_clock::duration exportTime{};
    uint64_t nbSpansInRound = 0;
    for ([[maybe_unused]] auto iter : state) {
      RecordRequestSpan(telemetry, head);
      if (sampled && ++nbSpansInRound == kSpansPerExport) {
        state.PauseTiming();
        nbSpansInRound = 0;
        nbSpansEnded += kSpansPerExport;
        const auto exportStart = std::chrono::steady_clock::now();
        const bool exported = collector.waitForSpans(nbSpansEnded);
        exportTime += std::chrono::steady_clock::now() - exportStart;
        lastExport = CurrentAllocationTotals();
        state.ResumeTiming();
        if (!exported) {
          state.SkipWithError("a batch of spans was not exported within a second");
          break;
        }
      }
    }
    const AllocationTotals after = CurrentAllocationTotals();
    nbSpansEnded += nbSpansInRound;

    state.counters["loop_allocs"] = benchmark::Counter(
        static_cast<double>(after.loopAllocations - before.loopAllocations), benchmark::Counter::kAvgIterations);
    state.counters["loop_bytes"] =
        benchmark::Counter(static_cast<double>(after.loopBytes - before.loopBytes), benchmark::Counter::kAvgIterations);
    // Per span of the rounds exported in the timed loop.
    const uint64_t nbSpansExported = nbSpansEnded - nbSpansInRound - (sampled ? kWarmUpRounds * kSpansPerExport : 0);
    const double exportDivisor = nbSpansExported == 0 ? 1.0 : static_cast<double>(nbSpansExported);
    state.counters["export_allocs"] =
        static_cast<double>(lastExport.otherAllocations - before.otherAllocations) / exportDivisor;
    state.counters["export_ns"] =
        static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(exportTime).count()) / exportDivisor;
  }  // The destruction of the context exports the spans of the last round.
  if (collector.nbSpans() != nbSpansEnded) {
    state.SkipWithError("the collector received " + std::to_string(collector.nbSpans()) + " spans out of " +
                        std::to_string(nbSpansEnded));
  }
}
BENCHMARK(BM_RequestSpan)->ArgNames({"sampled", "long_values"})->Args({0, 0})->Args({1, 0})->Args({1, 1});

}  // namespace
}  // namespace aeronet

// Interposed on the allocator of every library of the process: count, then forward to glibc. aligned_alloc and
// posix_memalign are not counted (no aligned allocation in the span path).
extern "C" void* malloc(std::size_t size) noexcept {
  aeronet::CountAllocation(size);
  return __libc_malloc(size);
}

extern "C" void* calloc(std::size_t nmemb, std::size_t size) noexcept {
  aeronet::CountAllocation(nmemb * size);
  return __libc_calloc(nmemb, size);
}

extern "C" void* realloc(void* ptr, std::size_t size) noexcept {
  aeronet::CountAllocation(size);
  return __libc_realloc(ptr, size);
}

BENCHMARK_MAIN();
