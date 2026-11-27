// Micro-benchmark of a call of aeronet::log, filtered out by the level (the debug calls on the request paths) and
// written, against the same call made through spdlog directly. The written messages go to a logger without output
// (null sink), so that the formatting and the dispatch are measured, not the terminal.
#include <benchmark/benchmark.h>

#include "aeronet/log.hpp"

#ifdef AERONET_ENABLE_SPDLOG
#include <spdlog/logger.h>
#include <spdlog/sinks/null_sink.h>
#include <spdlog/spdlog.h>

#include <memory>
#endif

namespace aeronet {
namespace {

void BM_AeronetLogFiltered(benchmark::State& state) {
  log::set_level(log::level::info);
  int value = 0;
  for (auto _ : state) {
    log::debug("filtered message {} {}", value, "text");
    benchmark::DoNotOptimize(++value);
  }
}
BENCHMARK(BM_AeronetLogFiltered);

#ifdef AERONET_ENABLE_SPDLOG

// The default logger of spdlog, without output, while alive.
class NullDefaultLogger {
 public:
  NullDefaultLogger() : _previous(spdlog::default_logger()) {
    spdlog::set_default_logger(spdlog::null_logger_mt("aeronet-log-bench"));
    log::set_level(log::level::info);
  }

  NullDefaultLogger(const NullDefaultLogger&) = delete;
  NullDefaultLogger(NullDefaultLogger&&) = delete;
  NullDefaultLogger& operator=(const NullDefaultLogger&) = delete;
  NullDefaultLogger& operator=(NullDefaultLogger&&) = delete;

  ~NullDefaultLogger() {
    spdlog::drop("aeronet-log-bench");
    spdlog::set_default_logger(_previous);
  }

 private:
  std::shared_ptr<spdlog::logger> _previous;
};

void BM_SpdlogFiltered(benchmark::State& state) {
  log::set_level(log::level::info);
  int value = 0;
  for (auto _ : state) {
    spdlog::debug("filtered message {} {}", value, "text");
    benchmark::DoNotOptimize(++value);
  }
}
BENCHMARK(BM_SpdlogFiltered);

void BM_AeronetLogWritten(benchmark::State& state) {
  const NullDefaultLogger nullLogger;
  int value = 0;
  for (auto _ : state) {
    log::info("written message {} {}", value, "text");
    benchmark::DoNotOptimize(++value);
  }
}
BENCHMARK(BM_AeronetLogWritten);

void BM_SpdlogWritten(benchmark::State& state) {
  const NullDefaultLogger nullLogger;
  int value = 0;
  for (auto _ : state) {
    spdlog::info("written message {} {}", value, "text");
    benchmark::DoNotOptimize(++value);
  }
}
BENCHMARK(BM_SpdlogWritten);

#endif

}  // namespace
}  // namespace aeronet

BENCHMARK_MAIN();
