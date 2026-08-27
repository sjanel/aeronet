#include <benchmark/benchmark.h>

#include <chrono>
#include <cstdint>

#include "aeronet/header-write.hpp"
#include "aeronet/internal/date-header-cache.hpp"
#include "aeronet/memory-utils-sv.hpp"
#include "aeronet/timedef.hpp"
#include "aeronet/timestring.hpp"

namespace aeronet {
namespace {

constexpr std::size_t kCRLFDateHeaderLen = http::CRLFDateHeaderSep.size() + RFC7231DateStrLen;
static_assert(kCRLFDateHeaderLen == http::HeaderSize(http::Date.size(), RFC7231DateStrLen));

void BM_SystemClockAndFormat(benchmark::State& state) {
  char out[kCRLFDateHeaderLen];

  for (auto _ : state) {
    TimeToStringRFC7231(SysClock::now(), AppendFixed<http::CRLFDateHeaderSep>(out));
    benchmark::DoNotOptimize(out);
    benchmark::ClobberMemory();
  }

  state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(kCRLFDateHeaderLen));
}

void BM_CachedCopy(benchmark::State& state) {
  internal::DateHeaderCache cache;
  cache.refreshIfNeeded(SteadyClock::now());
  char out[kCRLFDateHeaderLen];

  for (auto _ : state) {
    CopyCRLFDateHeader(cache.data(), out);
    benchmark::DoNotOptimize(out);
    benchmark::ClobberMemory();
  }

  state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(kCRLFDateHeaderLen));
}

void BM_CacheHitAndCopy(benchmark::State& state) {
  internal::DateHeaderCache cache;
  const auto steadyNow = SteadyClock::now();
  cache.refreshIfNeeded(steadyNow);
  char out[kCRLFDateHeaderLen];

  for (auto _ : state) {
    cache.refreshIfNeeded(steadyNow);
    CopyCRLFDateHeader(cache.data(), out);
    benchmark::DoNotOptimize(out);
    benchmark::ClobberMemory();
  }

  state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(kCRLFDateHeaderLen));
}

BENCHMARK(BM_SystemClockAndFormat)->Unit(benchmark::kNanosecond);
BENCHMARK(BM_CachedCopy)->Unit(benchmark::kNanosecond);
BENCHMARK(BM_CacheHitAndCopy)->Unit(benchmark::kNanosecond);

}  // namespace
}  // namespace aeronet

BENCHMARK_MAIN();
