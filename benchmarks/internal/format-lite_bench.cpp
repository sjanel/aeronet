// Micro-benchmark of aeronet::Format (aeronet/format-lite.hpp, the formatting of the aeronet logs and exception
// messages) against std::format, on the messages of the library: default formats of integers and strings, and integer
// format specifications, which format-lite writes itself, and the other format specifications, which it hands to
// std::format one field at a time.
#include <benchmark/benchmark.h>

#include <cstddef>
#include <format>
#include <string>
#include <string_view>

#include "aeronet/format-lite.hpp"

namespace aeronet {
namespace {

constexpr std::string_view kReason = "keep-alive timeout";

void BM_FormatLiteMessage(benchmark::State& state) {
  int fd = 0;
  std::size_t nbRequests = 1000;
  for (auto _ : state) {
    std::string message = Format("connection fd # {} closed after {} requests: {}", ++fd, nbRequests, kReason);
    benchmark::DoNotOptimize(message);
  }
}
BENCHMARK(BM_FormatLiteMessage);

void BM_StdFormatMessage(benchmark::State& state) {
  int fd = 0;
  std::size_t nbRequests = 1000;
  for (auto _ : state) {
    std::string message = std::format("connection fd # {} closed after {} requests: {}", ++fd, nbRequests, kReason);
    benchmark::DoNotOptimize(message);
  }
}
BENCHMARK(BM_StdFormatMessage);

void BM_FormatLiteSpecifications(benchmark::State& state) {
  unsigned events = 0;
  double ratio = 0.5;
  for (auto _ : state) {
    std::string message = Format("events=0x{:x} [{:>20}] ratio {:.2f}", ++events, kReason, ratio);
    benchmark::DoNotOptimize(message);
  }
}
BENCHMARK(BM_FormatLiteSpecifications);

void BM_StdFormatSpecifications(benchmark::State& state) {
  unsigned events = 0;
  double ratio = 0.5;
  for (auto _ : state) {
    std::string message = std::format("events=0x{:x} [{:>20}] ratio {:.2f}", ++events, kReason, ratio);
    benchmark::DoNotOptimize(message);
  }
}
BENCHMARK(BM_StdFormatSpecifications);

void BM_FormatLiteIntegerSpecifications(benchmark::State& state) {
  unsigned events = 0;
  int delta = -1;
  for (auto _ : state) {
    std::string message = Format("events={:#010x} delta={:+} mask={:08b}", ++events, --delta, events & 0xFFU);
    benchmark::DoNotOptimize(message);
  }
}
BENCHMARK(BM_FormatLiteIntegerSpecifications);

void BM_StdFormatIntegerSpecifications(benchmark::State& state) {
  unsigned events = 0;
  int delta = -1;
  for (auto _ : state) {
    std::string message = std::format("events={:#010x} delta={:+} mask={:08b}", ++events, --delta, events & 0xFFU);
    benchmark::DoNotOptimize(message);
  }
}
BENCHMARK(BM_StdFormatIntegerSpecifications);

}  // namespace
}  // namespace aeronet

BENCHMARK_MAIN();
