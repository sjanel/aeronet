#include <benchmark/benchmark.h>

#include <cstddef>
#include <string_view>

#include "aeronet/accept-encoding-negotiation.hpp"
#include "aeronet/compression-config.hpp"

namespace aeronet {
namespace {

// Accept-Encoding of the current browsers.
constexpr std::string_view kBrowserValue = "gzip, deflate, br, zstd";
// Same options, in another order: alternating them misses the cache of the last value on each request.
constexpr std::string_view kOtherValue = "deflate, gzip, br, zstd";
// Too long to be cached.
constexpr std::string_view kLongValue = "gzip;q=1.0, deflate;q=0.6, br;q=0.9, zstd;q=0.95, identity;q=0.1";
static_assert(kLongValue.size() > EncodingSelector::kMaxCachedAcceptEncodingSize);

// Requests of clients sending the same value, the common case: negotiated once, then answered by the cache.
void BM_NegotiateSameValue(benchmark::State& state) {
  EncodingSelector selector{CompressionConfig{}};
  for (auto _ : state) {
    benchmark::DoNotOptimize(selector.negotiateAcceptEncoding(kBrowserValue));
  }
}

// Requests of clients sending two values in turn: each one is negotiated.
void BM_NegotiateAlternatingValues(benchmark::State& state) {
  EncodingSelector selector{CompressionConfig{}};
  std::size_t pos = 0;
  for (auto _ : state) {
    benchmark::DoNotOptimize(selector.negotiateAcceptEncoding((++pos % 2) == 0 ? kBrowserValue : kOtherValue));
  }
}

void BM_NegotiateLongValue(benchmark::State& state) {
  EncodingSelector selector{CompressionConfig{}};
  for (auto _ : state) {
    benchmark::DoNotOptimize(selector.negotiateAcceptEncoding(kLongValue));
  }
}

BENCHMARK(BM_NegotiateSameValue)->Unit(benchmark::kNanosecond);
BENCHMARK(BM_NegotiateAlternatingValues)->Unit(benchmark::kNanosecond);
BENCHMARK(BM_NegotiateLongValue)->Unit(benchmark::kNanosecond);

}  // namespace
}  // namespace aeronet

BENCHMARK_MAIN();
