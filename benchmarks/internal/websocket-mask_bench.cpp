// Microbenchmark for WebSocket payload (un)masking (XOR with 4-byte repeating key).
//
// Measures throughput of `aeronet::websocket::ApplyMask()` across payload sizes
// covering the SIMD fast path (AVX2 / SSE2 / NEON) and the scalar tail.
// Useful to validate vectorization changes without going through k6 / network.

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <random>
#include <span>

#include "aeronet/websocket-frame.hpp"

namespace {

auto MakePayload(std::size_t sz) {
  auto data = std::make_unique_for_overwrite<std::byte[]>(sz);
  std::mt19937_64 rng(0xC0FFEE);
  for (std::size_t i = 0; i < sz; ++i) {
    data[i] = static_cast<std::byte>(rng() & 0xFF);
  }
  return data;
}

void BM_ApplyMask(benchmark::State& state) {
  const auto sz = static_cast<std::size_t>(state.range(0));
  auto data = MakePayload(sz);
  static constexpr aeronet::websocket::MaskingKey key = 0xDEADBEEFU;

  for ([[maybe_unused]] auto iter : state) {
    aeronet::websocket::ApplyMask(std::span<std::byte>(data.get(), sz), key);
    benchmark::DoNotOptimize(data.get());
  }

  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(sz));
}

}  // namespace

BENCHMARK(BM_ApplyMask)
    ->Arg(16)
    ->Arg(64)
    ->Arg(256)
    ->Arg(1024)
    ->Arg(4096)
    ->Arg(16384)
    ->Arg(65536)
    ->Arg(262144)
    ->Arg(1048576);

BENCHMARK_MAIN();
