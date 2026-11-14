#include <benchmark/benchmark.h>

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

// The copy of the formatted date alone: the cost of a response before the Date of each response read its second.
void BM_CachedCopy(benchmark::State& state) {
  internal::DateHeaderCache cache;
  const char* header = cache.current();
  char out[kCRLFDateHeaderLen];

  for (auto _ : state) {
    CopyCRLFDateHeader(header, out);
    benchmark::DoNotOptimize(out);
    benchmark::ClobberMemory();
  }

  state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(kCRLFDateHeaderLen));
}

// What each response pays: a read of the wall clock in whole seconds, and the copy of the date of that second.
void BM_CurrentAndCopy(benchmark::State& state) {
  internal::DateHeaderCache cache;
  char out[kCRLFDateHeaderLen];

  for (auto _ : state) {
    CopyCRLFDateHeader(cache.current(), out);
    benchmark::DoNotOptimize(out);
    benchmark::ClobberMemory();
  }

  state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(kCRLFDateHeaderLen));
}

BENCHMARK(BM_SystemClockAndFormat)->Unit(benchmark::kNanosecond);
BENCHMARK(BM_CachedCopy)->Unit(benchmark::kNanosecond);
BENCHMARK(BM_CurrentAndCopy)->Unit(benchmark::kNanosecond);

}  // namespace
}  // namespace aeronet

BENCHMARK_MAIN();
