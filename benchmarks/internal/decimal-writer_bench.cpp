// bench_decimal_writer.cpp
//
// Google Benchmark suite comparing aeronet's WriteU16/WriteU32/WriteU64/WriteInt
// (decimal_writer.hpp) against std::to_chars from <charconv>.
//
// What is covered:
//   1. Full-range random values per type   -> realistic "average" workload where
//                                              digit counts vary run to run.
//   2. Fixed digit-length buckets (u32/u64) -> isolates how each implementation
//                                              scales with the number of digits
//                                              actually produced (1 digit vs 19).
//   3. Edge cases (0, min, max)             -> including INT_MIN, which exercises
//                                              the "-(val+1)+1" overflow-safe path
//                                              in WriteSInt.
//   4. Multi-field composite                -> mirrors the "id,length,port" style
//                                              usage shown in decimal_writer.hpp's
//                                              doc comment.
//   5. Dispatcher overhead                  -> WriteU32 called directly vs through
//                                              the generic WriteInt/WriteUInt
//                                              templates, to confirm the
//                                              constexpr-if dispatch is free.

#include <benchmark/benchmark.h>

#include <charconv>
#include <cstdint>
#include <limits>
#include <random>
#include <string>
#include <type_traits>
#include <vector>

#include "aeronet/decimal-writer.hpp"
#include "aeronet/ndigits.hpp"

namespace aeronet {
namespace {

// Buffer large enough for any 64-bit integer in decimal, plus a sign and margin.
constexpr std::size_t kBufSize = 32;

// Number of pre-generated values cycled through during each benchmark. Kept as
// a power of two so we can advance the index with a cheap mask instead of a
// modulo, and large enough to defeat branch predictors that could otherwise
// learn a too-short repeating pattern.
constexpr std::size_t kDataSetSize = 1024;

// Generates `count` random values of type T uniformly distributed in [lo, hi].
// Values are precomputed outside the timed loop so the RNG never pollutes the
// measurement.
template <typename T>
std::vector<T> MakeRandomValues(std::size_t count, T lo, T hi, uint32_t seed = 42) {
  std::vector<T> values;
  values.reserve(count);
  std::mt19937_64 rng(seed);
  if constexpr (std::is_signed_v<T>) {
    std::uniform_int_distribution<int64_t> dist(static_cast<int64_t>(lo), static_cast<int64_t>(hi));
    for (std::size_t i = 0; i < count; ++i) {
      values.push_back(static_cast<T>(dist(rng)));
    }
  } else {
    std::uniform_int_distribution<uint64_t> dist(static_cast<uint64_t>(lo), static_cast<uint64_t>(hi));
    for (std::size_t i = 0; i < count; ++i) {
      values.push_back(static_cast<T>(dist(rng)));
    }
  }
  return values;
}

}  // namespace

// -----------------------------------------------------------------------------
// 1. Full-range random values: realistic mixed workload.
// -----------------------------------------------------------------------------

template <typename T>
static void BM_WriteInt_Random(benchmark::State& state) {
  auto values = MakeRandomValues<T>(kDataSetSize, std::numeric_limits<T>::min(), std::numeric_limits<T>::max());
  char buf[kBufSize];
  std::size_t idx = 0;
  for (auto _ : state) {
    char* end = WriteInt(buf, values[idx], ndigits(values[idx]));
    benchmark::DoNotOptimize(buf);
    benchmark::DoNotOptimize(end);
    idx = (idx + 1) & (kDataSetSize - 1);
  }
  state.SetItemsProcessed(state.iterations());
}

template <typename T>
static void BM_ToChars_Random(benchmark::State& state) {
  auto values = MakeRandomValues<T>(kDataSetSize, std::numeric_limits<T>::min(), std::numeric_limits<T>::max());
  char buf[kBufSize];
  std::size_t idx = 0;
  for (auto _ : state) {
    auto res = std::to_chars(buf, buf + kBufSize, values[idx]);
    benchmark::DoNotOptimize(buf);
    benchmark::DoNotOptimize(res.ptr);
    idx = (idx + 1) & (kDataSetSize - 1);
  }
  state.SetItemsProcessed(state.iterations());
}

BENCHMARK_TEMPLATE(BM_WriteInt_Random, uint16_t);
BENCHMARK_TEMPLATE(BM_ToChars_Random, uint16_t);

BENCHMARK_TEMPLATE(BM_WriteInt_Random, uint32_t);
BENCHMARK_TEMPLATE(BM_ToChars_Random, uint32_t);

BENCHMARK_TEMPLATE(BM_WriteInt_Random, uint64_t);
BENCHMARK_TEMPLATE(BM_ToChars_Random, uint64_t);

BENCHMARK_TEMPLATE(BM_WriteInt_Random, int16_t);
BENCHMARK_TEMPLATE(BM_ToChars_Random, int16_t);

BENCHMARK_TEMPLATE(BM_WriteInt_Random, int32_t);
BENCHMARK_TEMPLATE(BM_ToChars_Random, int32_t);

BENCHMARK_TEMPLATE(BM_WriteInt_Random, int64_t);
BENCHMARK_TEMPLATE(BM_ToChars_Random, int64_t);

// -----------------------------------------------------------------------------
// 2. Fixed digit-length buckets: how does each implementation scale with the
//    number of digits actually produced? Naive divide-by-10 loops and
//    chunked/table-driven algorithms tend to have very different profiles for
//    1-digit vs 19-digit numbers.
// -----------------------------------------------------------------------------

namespace digit_ranges {

struct BoundsU64 {
  uint8_t digits;
  uint64_t lo;
  uint64_t hi;
};

constexpr BoundsU64 kU64Bounds[] = {
    {1, 0ULL, 9ULL},
    {2, 10ULL, 99ULL},
    {4, 1000ULL, 9999ULL},
    {6, 100000ULL, 999999ULL},
    {9, 100000000ULL, 999999999ULL},
    {13, 1000000000000ULL, 9999999999999ULL},
    {19, 1000000000000000000ULL, 9999999999999999999ULL},
};

struct BoundsU32 {
  uint8_t digits;
  uint32_t lo;
  uint32_t hi;
};

constexpr BoundsU32 kU32Bounds[] = {
    {1, 0u, 9u}, {3, 100u, 999u}, {5, 10000u, 99999u}, {7, 1000000u, 9999999u}, {10, 1000000000u, 4294967295u},
};

}  // namespace digit_ranges

static void BM_WriteU64_FixedDigits(benchmark::State& state) {
  const auto& b = digit_ranges::kU64Bounds[state.range(0)];
  state.SetLabel(std::to_string(b.digits) + "_digits");
  auto values = MakeRandomValues<uint64_t>(kDataSetSize, b.lo, b.hi);
  char buf[kBufSize];
  std::size_t idx = 0;
  for (auto _ : state) {
    char* end = WriteU64(buf, values[idx], b.digits);
    benchmark::DoNotOptimize(buf);
    benchmark::DoNotOptimize(end);
    idx = (idx + 1) & (kDataSetSize - 1);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_WriteU64_FixedDigits)->DenseRange(0, 6);

static void BM_ToCharsU64_FixedDigits(benchmark::State& state) {
  const auto& b = digit_ranges::kU64Bounds[state.range(0)];
  state.SetLabel(std::to_string(b.digits) + "_digits");
  auto values = MakeRandomValues<uint64_t>(kDataSetSize, b.lo, b.hi);
  char buf[kBufSize];
  std::size_t idx = 0;
  for (auto _ : state) {
    auto res = std::to_chars(buf, buf + kBufSize, values[idx]);
    benchmark::DoNotOptimize(buf);
    benchmark::DoNotOptimize(res.ptr);
    idx = (idx + 1) & (kDataSetSize - 1);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ToCharsU64_FixedDigits)->DenseRange(0, 6);

static void BM_WriteU32_FixedDigits(benchmark::State& state) {
  const auto& b = digit_ranges::kU32Bounds[state.range(0)];
  state.SetLabel(std::to_string(b.digits) + "_digits");
  auto values = MakeRandomValues<uint32_t>(kDataSetSize, b.lo, b.hi);
  char buf[kBufSize];
  std::size_t idx = 0;
  for (auto _ : state) {
    char* end = WriteU32(buf, values[idx], b.digits);
    benchmark::DoNotOptimize(buf);
    benchmark::DoNotOptimize(end);
    idx = (idx + 1) & (kDataSetSize - 1);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_WriteU32_FixedDigits)->DenseRange(0, 4);

static void BM_ToCharsU32_FixedDigits(benchmark::State& state) {
  const auto& b = digit_ranges::kU32Bounds[state.range(0)];
  state.SetLabel(std::to_string(b.digits) + "_digits");
  auto values = MakeRandomValues<uint32_t>(kDataSetSize, b.lo, b.hi);
  char buf[kBufSize];
  std::size_t idx = 0;
  for (auto _ : state) {
    auto res = std::to_chars(buf, buf + kBufSize, values[idx]);
    benchmark::DoNotOptimize(buf);
    benchmark::DoNotOptimize(res.ptr);
    idx = (idx + 1) & (kDataSetSize - 1);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ToCharsU32_FixedDigits)->DenseRange(0, 4);

// -----------------------------------------------------------------------------
// 3. Edge cases: 0, type max, type min. The signed min cases exercise the
//    overflow-safe "-(val+1)+1" branch in WriteSInt.
// -----------------------------------------------------------------------------

// NOTE: `benchmark::DoNotOptimize(value)` inside the loop is not decorative
// here. `value` is a true loop-invariant. WriteInt/WriteU32/WriteU64 live in a
// separate translation unit (decimal_writer.cpp), so the optimizer can never
// see through the call and cannot hoist anything. std::to_chars, being a
// template defined in <charconv> and therefore fully visible in *this* TU,
// CAN be proven loop-invariant by the compiler once `value` never changes --
// without the DoNotOptimize below, -O3 will happily hoist or fold the whole
// conversion out of the loop, leaving only the forced store to `buf`. That
// produces artificially flat ~0.4ns numbers for std::to_chars regardless of
// the value's actual magnitude, which is not a real result. Marking `value`
// as "escaped" on every iteration forces the compiler to treat it as opaque,
// giving both implementations a fair, non-hoistable measurement.
#define EDGE_BENCH(NAME, TYPE, VALUE_EXPR)                  \
  static void BM_WriteInt_##NAME(benchmark::State& state) { \
    TYPE value = (VALUE_EXPR);                              \
    char buf[kBufSize];                                     \
    for (auto _ : state) {                                  \
      benchmark::DoNotOptimize(value);                      \
      char* end = WriteInt(buf, value, ndigits(value));     \
      benchmark::DoNotOptimize(buf);                        \
      benchmark::DoNotOptimize(end);                        \
    }                                                       \
  }                                                         \
  BENCHMARK(BM_WriteInt_##NAME);                            \
  static void BM_ToChars_##NAME(benchmark::State& state) {  \
    TYPE value = (VALUE_EXPR);                              \
    char buf[kBufSize];                                     \
    for (auto _ : state) {                                  \
      benchmark::DoNotOptimize(value);                      \
      auto res = std::to_chars(buf, buf + kBufSize, value); \
      benchmark::DoNotOptimize(buf);                        \
      benchmark::DoNotOptimize(res.ptr);                    \
    }                                                       \
  }                                                         \
  BENCHMARK(BM_ToChars_##NAME)

EDGE_BENCH(U16_Zero, uint16_t, 0);
EDGE_BENCH(U16_Max, uint16_t, std::numeric_limits<uint16_t>::max());

EDGE_BENCH(U32_Zero, uint32_t, 0);
EDGE_BENCH(U32_Max, uint32_t, std::numeric_limits<uint32_t>::max());

EDGE_BENCH(U64_Zero, uint64_t, 0);
EDGE_BENCH(U64_Max, uint64_t, std::numeric_limits<uint64_t>::max());

EDGE_BENCH(I16_Zero, int16_t, 0);
EDGE_BENCH(I16_Max, int16_t, std::numeric_limits<int16_t>::max());
EDGE_BENCH(I16_Min, int16_t, std::numeric_limits<int16_t>::min());

EDGE_BENCH(I32_Zero, int32_t, 0);
EDGE_BENCH(I32_Max, int32_t, std::numeric_limits<int32_t>::max());
EDGE_BENCH(I32_Min, int32_t, std::numeric_limits<int32_t>::min());

EDGE_BENCH(I64_Zero, int64_t, 0);
EDGE_BENCH(I64_Max, int64_t, std::numeric_limits<int64_t>::max());
EDGE_BENCH(I64_Min, int64_t, std::numeric_limits<int64_t>::min());

#undef EDGE_BENCH

// -----------------------------------------------------------------------------
// 4. Multi-field composite: mirrors the "id,length,port" usage example from
//    decimal_writer.hpp's doc comment, exercising WriteU32/WriteU64/WriteU16
//    back to back into one buffer.
// -----------------------------------------------------------------------------

static void BM_WriteInt_MultiField(benchmark::State& state) {
  auto ids = MakeRandomValues<uint32_t>(kDataSetSize, 0u, std::numeric_limits<uint32_t>::max());
  auto lengths = MakeRandomValues<uint64_t>(kDataSetSize, 0ULL, std::numeric_limits<uint64_t>::max());
  auto ports = MakeRandomValues<uint16_t>(kDataSetSize, 0u, std::numeric_limits<uint16_t>::max());
  char buf[128];
  std::size_t idx = 0;
  for (auto _ : state) {
    char* p = buf;
    p = WriteU32(p, ids[idx], ndigits(ids[idx]));
    *p++ = ',';
    p = WriteU64(p, lengths[idx], ndigits(lengths[idx]));
    *p++ = ',';
    p = WriteU16(p, ports[idx], ndigits(ports[idx]));
    benchmark::DoNotOptimize(buf);
    benchmark::DoNotOptimize(p);
    idx = (idx + 1) & (kDataSetSize - 1);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_WriteInt_MultiField);

static void BM_ToChars_MultiField(benchmark::State& state) {
  auto ids = MakeRandomValues<uint32_t>(kDataSetSize, 0u, std::numeric_limits<uint32_t>::max());
  auto lengths = MakeRandomValues<uint64_t>(kDataSetSize, 0ULL, std::numeric_limits<uint64_t>::max());
  auto ports = MakeRandomValues<uint16_t>(kDataSetSize, 0u, std::numeric_limits<uint16_t>::max());
  char buf[128];
  std::size_t idx = 0;
  for (auto _ : state) {
    char* p = buf;
    p = std::to_chars(p, buf + 128, ids[idx]).ptr;
    *p++ = ',';
    p = std::to_chars(p, buf + 128, lengths[idx]).ptr;
    *p++ = ',';
    p = std::to_chars(p, buf + 128, ports[idx]).ptr;
    benchmark::DoNotOptimize(buf);
    benchmark::DoNotOptimize(p);
    idx = (idx + 1) & (kDataSetSize - 1);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ToChars_MultiField);

// -----------------------------------------------------------------------------
// 5. Dispatcher overhead: WriteU32 called directly vs through the generic
//    WriteInt -> WriteUInt constexpr-if dispatch chain. Should be a wash,
//    since the dispatch is resolved entirely at compile time.
// -----------------------------------------------------------------------------

static void BM_WriteU32_Direct(benchmark::State& state) {
  auto values = MakeRandomValues<uint32_t>(kDataSetSize, 0u, std::numeric_limits<uint32_t>::max());
  char buf[kBufSize];
  std::size_t idx = 0;
  for (auto _ : state) {
    const auto val = values[idx];
    char* end = WriteU32(buf, val, ndigits(val));
    benchmark::DoNotOptimize(buf);
    benchmark::DoNotOptimize(end);
    idx = (idx + 1) & (kDataSetSize - 1);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_WriteU32_Direct);

static void BM_WriteU32_ViaWriteInt(benchmark::State& state) {
  auto values = MakeRandomValues<uint32_t>(kDataSetSize, 0u, std::numeric_limits<uint32_t>::max());
  char buf[kBufSize];
  std::size_t idx = 0;
  for (auto _ : state) {
    char* end = WriteInt(buf, values[idx], ndigits(values[idx]));
    benchmark::DoNotOptimize(buf);
    benchmark::DoNotOptimize(end);
    idx = (idx + 1) & (kDataSetSize - 1);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_WriteU32_ViaWriteInt);
}  // namespace aeronet

BENCHMARK_MAIN();