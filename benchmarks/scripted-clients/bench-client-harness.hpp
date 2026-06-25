#pragma once

// Shared harness for the scripted-client benchmarks.
//
// Each client driver (aeronet, libcurl, drogon, beast, ...) only implements a small `Session` type and
// hands it to AERONET_CLIENT_BENCH_MAIN. This header owns everything else: uniform CLI parsing, the
// multi-threaded timing loop, a bounded-memory latency histogram and the JSON / human-readable output.
//
// Concurrency model (fair across very different client libraries): the harness spawns `--threads` worker
// threads, each owning its own `Session` (one client instance + one keep-alive connection) and running a
// synchronous request/response loop. This is the natural mode of every client compared here (aeronet's
// HttpClient is one-instance-per-thread; libcurl easy, drogon sync sendRequest and beast sync I/O are all
// blocking). The server (a separate aeronet-bench-server with many threads) is never the bottleneck, so the
// measured cost is the client's: request serialization, response parsing and buffer/allocation management.
//
// Session contract (implemented by each driver):
//
//   class Session {
//    public:
//     Session(const aeronet::bench::ClientBenchConfig& cfg, const aeronet::bench::ScenarioSpec& spec);
//     // Perform exactly one request for the cached scenario. Return the number of response *body* bytes
//     // received on success, or a negative value on error.
//     long doRequest();
//   };

#include <atomic>
#include <bit>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/resource.h>
#endif

namespace aeronet::bench {

using Clock = std::chrono::steady_clock;

// ----------------------------- Scenario ----------------------------- //

// A fully resolved scenario: method + request target + (optional) body + extra request headers +
// content-coding policy + connection-reuse policy.
struct ScenarioSpec {
  std::string name;
  std::string method;  // "GET" or "POST"
  std::string path;    // origin-form target, e.g. "/ping" or "/body?size=1048576"
  std::string body;    // request body (POST only)
  // Extra request headers the client must serialize (stresses the request-header build path).
  std::vector<std::pair<std::string, std::string>> requestHeaders;
  // Advertised Accept-Encoding. "identity" keeps the server from compressing (apples-to-apples); "gzip"
  // asks the server to gzip the response, which the client must then decode (see `decode`).
  std::string acceptEncoding{"identity"};
  bool decode{false};  // client decodes a gzip'd response body and counts the *decoded* bytes
  bool reuse{true};    // keep-alive reuse across requests (false => fresh connection per request)
};

// ----------------------------- Config ------------------------------- //

struct ClientBenchConfig {
  std::string baseUrl{"http://127.0.0.1:8080"};  // "scheme://host:port"
  std::string clientName;                         // filled in by the driver
  std::string scenario{"small-get"};
  std::size_t bodySize{1UL << 20};  // bytes, used by large-get (response) and post (request)
  std::uint32_t threads{1};
  std::chrono::milliseconds duration{std::chrono::seconds{10}};
  std::chrono::milliseconds warmup{std::chrono::seconds{2}};
  bool json{false};
};

// Parse "10s" / "500ms" / "250" (bare number => seconds) into milliseconds.
inline std::chrono::milliseconds ParseDuration(std::string_view sv) {
  std::uint64_t value = 0;
  auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), value);
  std::string_view unit(ptr, sv.data() + sv.size());
  if (ec != std::errc{}) {
    return std::chrono::seconds{0};
  }
  if (unit == "ms") {
    return std::chrono::milliseconds{value};
  }
  if (unit == "m") {
    return std::chrono::minutes{value};
  }
  // "s" or bare number
  return std::chrono::seconds{value};
}

inline void PrintUsage(const char* prog) {
  std::fprintf(stderr,
               "Usage: %s [options]\n"
               "  --url URL          Base URL scheme://host:port (default http://127.0.0.1:8080)\n"
               "  --scenario NAME    small-get | large-get | post | json | no-reuse (default small-get)\n"
               "  --body-size N      Body size in bytes for large-get/post (default 1048576)\n"
               "  --threads N        Concurrent worker threads/connections (default 1)\n"
               "  --duration D       Measured window, e.g. 10s / 500ms (default 10s)\n"
               "  --warmup D         Warmup window before measuring (default 2s)\n"
               "  --json             Emit a single-line JSON result (machine readable)\n"
               "  --help             Show this help\n",
               prog);
}

inline ClientBenchConfig ParseArgs(int argc, char** argv) {
  ClientBenchConfig cfg;
  for (int i = 1; i < argc; ++i) {
    std::string_view arg(argv[i]);
    auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : nullptr; };
    if ((arg == "--url" || arg == "--base-url")) {
      if (const char* v = next()) {
        cfg.baseUrl = v;
      }
    } else if (arg == "--scenario") {
      if (const char* v = next()) {
        cfg.scenario = v;
      }
    } else if (arg == "--body-size") {
      if (const char* v = next()) {
        cfg.bodySize = std::strtoull(v, nullptr, 10);
      }
    } else if (arg == "--threads") {
      if (const char* v = next()) {
        cfg.threads = static_cast<std::uint32_t>(std::strtoul(v, nullptr, 10));
      }
    } else if (arg == "--duration") {
      if (const char* v = next()) {
        cfg.duration = ParseDuration(v);
      }
    } else if (arg == "--warmup") {
      if (const char* v = next()) {
        cfg.warmup = ParseDuration(v);
      }
    } else if (arg == "--json") {
      cfg.json = true;
    } else if (arg == "--help" || arg == "-h") {
      PrintUsage(argv[0]);
      std::exit(0);
    }
  }
  if (cfg.threads == 0) {
    cfg.threads = 1;
  }
  return cfg;
}

// Map a scenario name to a concrete ScenarioSpec. Unknown names fall back to small-get.
inline ScenarioSpec MakeScenario(const ClientBenchConfig& cfg) {
  const std::string& name = cfg.scenario;
  ScenarioSpec sc;
  sc.name = name;
  sc.method = "GET";
  if (name == "large-get") {
    sc.path = "/body?size=" + std::to_string(cfg.bodySize);  // big response payload (transfer-bound)
    return sc;
  }
  if (name == "headers") {
    // Many request headers (client serialization) + many response headers (client parsing).
    sc.path = "/headers?count=32&size=64";
    for (int i = 0; i < 24; ++i) {
      sc.requestHeaders.emplace_back("X-Bench-Req-" + std::to_string(i),
                                     "payload-value-for-bench-request-header-" + std::to_string(i));
    }
    return sc;
  }
  if (name == "post") {
    sc.method = "POST";
    sc.path = "/uppercase";
    sc.body.assign(cfg.bodySize, 'a');
    return sc;
  }
  if (name == "json") {
    sc.path = "/json?items=200";
    return sc;
  }
  if (name == "compress") {
    // Highly compressible JSON: the server gzips it and the client must decode (aeronet natively, the
    // others via the shared zlib-ng helper). Measures content-coding throughput.
    sc.path = "/json?items=800";
    sc.acceptEncoding = "gzip";
    sc.decode = true;
    return sc;
  }
  if (name == "no-reuse") {
    sc.path = "/ping";
    sc.reuse = false;
    return sc;
  }
  // small-get (default)
  sc.name = "small-get";
  sc.path = "/ping";
  return sc;
}

// ----------------------- Latency histogram -------------------------- //

// HdrHistogram-lite: 16 linear sub-buckets per power-of-two, recorded in nanoseconds. ~6% relative error,
// bounded memory (kSize * 8 bytes), and a branch-light record() so it does not perturb the measurement.
class LatencyHistogram {
 public:
  static constexpr int kSubBits = 4;
  static constexpr int kSub = 1 << kSubBits;  // 16
  static constexpr int kGroups = 60;          // exponents 0..59 -> values up to ~2^60 ns
  static constexpr int kSize = kGroups * kSub;

  void record(std::uint64_t ns) noexcept {
    ++_counts[indexOf(ns)];
    _sum += ns;
    ++_count;
    if (ns > _max) {
      _max = ns;
    }
  }

  void merge(const LatencyHistogram& other) noexcept {
    for (int i = 0; i < kSize; ++i) {
      _counts[i] += other._counts[i];
    }
    _sum += other._sum;
    _count += other._count;
    if (other._max > _max) {
      _max = other._max;
    }
  }

  [[nodiscard]] std::uint64_t count() const noexcept { return _count; }
  [[nodiscard]] std::uint64_t maxNs() const noexcept { return _max; }
  [[nodiscard]] double meanNs() const noexcept {
    return _count != 0 ? static_cast<double>(_sum) / static_cast<double>(_count) : 0.0;
  }

  // Value (ns) at the given percentile in [0, 100].
  [[nodiscard]] std::uint64_t percentileNs(double pct) const noexcept {
    if (_count == 0) {
      return 0;
    }
    const auto target = static_cast<std::uint64_t>(pct / 100.0 * static_cast<double>(_count));
    std::uint64_t cumulative = 0;
    for (int i = 0; i < kSize; ++i) {
      cumulative += _counts[i];
      if (cumulative >= target) {
        return valueOf(i);
      }
    }
    return _max;
  }

 private:
  static int indexOf(std::uint64_t v) noexcept {
    if (v < static_cast<std::uint64_t>(kSub)) {
      return static_cast<int>(v);  // 0..15 are exact
    }
    const int exponent = 63 - std::countl_zero(v);                       // >= 4
    const std::uint64_t sub = (v >> (exponent - kSubBits)) - kSub;       // 0..kSub-1
    const int idx = (exponent - kSubBits + 1) * kSub + static_cast<int>(sub);
    return idx < kSize ? idx : kSize - 1;
  }

  static std::uint64_t valueOf(int idx) noexcept {  // lower bound of bucket idx
    if (idx < kSub) {
      return static_cast<std::uint64_t>(idx);
    }
    const int group = idx / kSub;  // >= 1
    const int sub = idx % kSub;
    return (static_cast<std::uint64_t>(kSub) + static_cast<std::uint64_t>(sub)) << (group - 1);
  }

  std::uint64_t _counts[kSize]{};
  std::uint64_t _sum{0};
  std::uint64_t _count{0};
  std::uint64_t _max{0};
};

// ----------------------------- Result ------------------------------- //

struct BenchResult {
  std::string client;
  std::string scenario;
  std::uint32_t threads{0};
  std::uint64_t requests{0};
  std::uint64_t errors{0};
  std::uint64_t bytes{0};
  double durationSeconds{0.0};
  double rps{0.0};
  double bytesPerSecond{0.0};
  double avgUs{0.0};
  double p50Us{0.0};
  double p90Us{0.0};
  double p99Us{0.0};
  double maxUs{0.0};
  long rssKb{0};
};

inline long PeakRssKb() {
#if defined(__unix__) || defined(__APPLE__)
  struct rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) == 0) {
#if defined(__APPLE__)
    return usage.ru_maxrss / 1024;  // macOS reports bytes
#else
    return usage.ru_maxrss;  // Linux reports KB
#endif
  }
#endif
  return 0;
}

inline void PrintResult(const BenchResult& r) {
  std::printf(
      "\n  %-10s  scenario=%-9s threads=%u\n"
      "    requests : %llu (errors: %llu) over %.2fs\n"
      "    rps      : %.0f\n"
      "    transfer : %.2f MB/s\n"
      "    latency  : avg %.1fus  p50 %.1fus  p90 %.1fus  p99 %.1fus  max %.1fus\n"
      "    peak RSS : %ld KB\n",
      r.client.c_str(), r.scenario.c_str(), r.threads, static_cast<unsigned long long>(r.requests),
      static_cast<unsigned long long>(r.errors), r.durationSeconds, r.rps, r.bytesPerSecond / (1024.0 * 1024.0),
      r.avgUs, r.p50Us, r.p90Us, r.p99Us, r.maxUs, r.rssKb);
}

inline void PrintResultJson(const BenchResult& r) {
  std::printf(
      "{\"client\":\"%s\",\"scenario\":\"%s\",\"threads\":%u,\"requests\":%llu,\"errors\":%llu,"
      "\"bytes\":%llu,\"duration_s\":%.3f,\"rps\":%.1f,\"bytes_per_s\":%.1f,\"avg_us\":%.3f,"
      "\"p50_us\":%.3f,\"p90_us\":%.3f,\"p99_us\":%.3f,\"max_us\":%.3f,\"rss_kb\":%ld}\n",
      r.client.c_str(), r.scenario.c_str(), r.threads, static_cast<unsigned long long>(r.requests),
      static_cast<unsigned long long>(r.errors), static_cast<unsigned long long>(r.bytes), r.durationSeconds,
      r.rps, r.bytesPerSecond, r.avgUs, r.p50Us, r.p90Us, r.p99Us, r.maxUs, r.rssKb);
}

// ----------------------------- Driver ------------------------------- //

template <class Session>
int RunClientBench(int argc, char** argv, std::string_view clientName) {
  ClientBenchConfig cfg = ParseArgs(argc, argv);
  cfg.clientName = std::string(clientName);
  const ScenarioSpec spec = MakeScenario(cfg);

  struct ThreadOut {
    LatencyHistogram hist;
    std::uint64_t requests{0};
    std::uint64_t bytes{0};
    std::uint64_t errors{0};
  };
  std::vector<ThreadOut> outs(cfg.threads);

  std::atomic<std::uint32_t> ready{0};
  std::atomic<bool> go{false};
  // Published by the main thread before `go` is set; read by workers only after observing go==true.
  Clock::time_point warmupEnd;
  Clock::time_point measuredEnd;

  std::vector<std::thread> workers;
  workers.reserve(cfg.threads);
  for (std::uint32_t t = 0; t < cfg.threads; ++t) {
    workers.emplace_back([&, t]() {
      ThreadOut& out = outs[t];
      try {
        Session session(cfg, spec);
        ready.fetch_add(1, std::memory_order_release);
        while (!go.load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }
        // Warmup: prime connections / JIT / page caches without recording.
        while (Clock::now() < warmupEnd) {
          (void)session.doRequest();
        }
        // Measured window.
        for (;;) {
          const auto begin = Clock::now();
          if (begin >= measuredEnd) {
            break;
          }
          long bytes = -1;
          try {
            bytes = session.doRequest();
          } catch (const std::exception&) {
            bytes = -1;
          }
          const auto end = Clock::now();
          if (bytes < 0) {
            ++out.errors;
            continue;
          }
          out.hist.record(static_cast<std::uint64_t>(
              std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count()));
          ++out.requests;
          out.bytes += static_cast<std::uint64_t>(bytes);
        }
      } catch (const std::exception& ex) {
        std::fprintf(stderr, "[%s] worker %u failed: %s\n", cfg.clientName.c_str(), t, ex.what());
        ready.fetch_add(1, std::memory_order_release);  // unblock main if construction threw
      }
    });
  }

  while (ready.load(std::memory_order_acquire) < cfg.threads) {
    std::this_thread::yield();
  }
  const auto start = Clock::now();
  warmupEnd = start + cfg.warmup;
  measuredEnd = warmupEnd + cfg.duration;
  go.store(true, std::memory_order_release);

  for (auto& worker : workers) {
    worker.join();
  }

  // Aggregate.
  BenchResult result;
  result.client = cfg.clientName;
  result.scenario = spec.name;
  result.threads = cfg.threads;
  LatencyHistogram merged;
  for (const ThreadOut& out : outs) {
    merged.merge(out.hist);
    result.requests += out.requests;
    result.bytes += out.bytes;
    result.errors += out.errors;
  }
  result.durationSeconds = std::chrono::duration<double>(cfg.duration).count();
  if (result.durationSeconds > 0.0) {
    result.rps = static_cast<double>(result.requests) / result.durationSeconds;
    result.bytesPerSecond = static_cast<double>(result.bytes) / result.durationSeconds;
  }
  constexpr double kNsToUs = 1e-3;
  result.avgUs = merged.meanNs() * kNsToUs;
  result.p50Us = static_cast<double>(merged.percentileNs(50.0)) * kNsToUs;
  result.p90Us = static_cast<double>(merged.percentileNs(90.0)) * kNsToUs;
  result.p99Us = static_cast<double>(merged.percentileNs(99.0)) * kNsToUs;
  result.maxUs = static_cast<double>(merged.maxNs()) * kNsToUs;
  result.rssKb = PeakRssKb();

  if (cfg.json) {
    PrintResultJson(result);
  } else {
    PrintResult(result);
  }
  return result.requests == 0 ? 1 : 0;
}

}  // namespace aeronet::bench

// Defines main() for a client driver. `SessionType` must satisfy the Session contract above.
#define AERONET_CLIENT_BENCH_MAIN(SessionType, ClientName)                  \
  int main(int argc, char** argv) {                                         \
    return ::aeronet::bench::RunClientBench<SessionType>(argc, argv, ClientName); \
  }
