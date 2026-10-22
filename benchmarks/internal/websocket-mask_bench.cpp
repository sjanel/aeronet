// Microbenchmark for WebSocket payload (un)masking (XOR with 4-byte repeating key).
//
// Measures throughput of `aeronet::websocket::ApplyMask()` across payload sizes
// covering the SIMD fast path (AVX2 / SSE2 / NEON) and the tail, and of the masked copy
// `AppendMasked()` (the fragments of a message copied to its reassembly buffer, the payload of a masked frame
// built by a client) against a copy followed by `ApplyMask()`.
// Useful to validate vectorization changes without going through k6 / network.

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <random>
#include <span>

#include "aeronet/memory-utils.hpp"
#include "aeronet/websocket-constants.hpp"
#include "aeronet/websocket-frame.hpp"

namespace {

constexpr aeronet::websocket::MaskingKey kKey = 0xDEADBEEFU;

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

  for ([[maybe_unused]] auto iter : state) {
    aeronet::websocket::ApplyMask(std::span<std::byte>(data.get(), sz), kKey);
    benchmark::DoNotOptimize(data.get());
  }

  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(sz));
}

// Masked copy in a single pass.
void BM_AppendMasked(benchmark::State& state) {
  const auto sz = static_cast<std::size_t>(state.range(0));
  const auto src = MakePayload(sz);
  const auto dst = std::make_unique_for_overwrite<std::byte[]>(sz);

  for ([[maybe_unused]] auto iter : state) {
    benchmark::DoNotOptimize(
        aeronet::websocket::AppendMasked(std::span<const std::byte>(src.get(), sz), dst.get(), kKey));
    benchmark::ClobberMemory();
  }

  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(sz));
}

// Masked copy in two passes: a copy, then masking in place.
void BM_CopyThenApplyMask(benchmark::State& state) {
  const auto sz = static_cast<std::size_t>(state.range(0));
  const auto src = MakePayload(sz);
  const auto dst = std::make_unique_for_overwrite<std::byte[]>(sz);

  for ([[maybe_unused]] auto iter : state) {
    aeronet::Copy(src.get(), sz, dst.get());
    aeronet::websocket::ApplyMask(std::span<std::byte>(dst.get(), sz), kKey);
    benchmark::DoNotOptimize(dst.get());
    benchmark::ClobberMemory();
  }

  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(sz));
}

// From a few bytes (the tail only) to large payloads (the SIMD loop only).
void Sizes(benchmark::Benchmark* bench) {
  for (const int64_t size : {1, 3, 7, 15, 16, 24, 31, 40, 64, 100, 256, 1024, 4096, 16384, 65536, 262144, 1048576}) {
    bench->Arg(size);
  }
}

}  // namespace

BENCHMARK(BM_ApplyMask)->Apply(Sizes);
BENCHMARK(BM_AppendMasked)->Apply(Sizes);
BENCHMARK(BM_CopyThenApplyMask)->Apply(Sizes);

BENCHMARK_MAIN();
