// Microbenchmark for aeronet::Copy / aeronet::Append (memory-utils.hpp).
//
// The HTTP request/response builders append a great many *short*, runtime-sized
// fragments (method, header names/values). When the size is a compile-time
// constant the compiler already lowers Copy() to direct stores; the interesting
// case is the runtime-sized one, where a naive std::memcpy lowers to a
// `call memcpy@PLT`. This benchmark compares that baseline against an inline
// overlapping-store strategy that avoids the call for small sizes.
//
// Two angles:
//   * BM_FixedSize  - one runtime-opaque size per run (best case for the branch
//                     predictor): isolates the per-size-class cost.
//   * BM_MixedHttp  - a realistic mix of method/header-name/header-value sizes in
//                     random order: exposes branch-misprediction cost.

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "aeronet/memory-utils.hpp"

namespace {

// ---------------------------------------------------------------------------
// Copy strategies under test
// ---------------------------------------------------------------------------

// Baseline: the pre-change behavior (plain memcpy => call memcpy@PLT for a
// runtime size).
inline void CopyMemcpy(std::string_view sv, char* dst) noexcept { std::memcpy(dst, sv.data(), sv.size()); }

// Overlapping fixed-width stores: no call for n <= 16. Reads/writes stay inside
// [ptr, ptr + n) so it is memory-safe for any caller (no padding/slack needed).
inline void CopyOverlap16(std::string_view sv, char* dst) noexcept {
  const char* src = sv.data();
  std::size_t n = sv.size();
  if (n > 16) {
    std::memcpy(dst, src, n);
    return;
  }
  if (n >= 8) {
    std::uint64_t lo;
    std::uint64_t hi;
    std::memcpy(&lo, src, 8);
    std::memcpy(&hi, src + n - 8, 8);
    std::memcpy(dst, &lo, 8);
    std::memcpy(dst + n - 8, &hi, 8);
  } else if (n >= 4) {
    std::uint32_t lo;
    std::uint32_t hi;
    std::memcpy(&lo, src, 4);
    std::memcpy(&hi, src + n - 4, 4);
    std::memcpy(dst, &lo, 4);
    std::memcpy(dst + n - 4, &hi, 4);
  } else if (n >= 2) {
    std::uint16_t lo;
    std::uint16_t hi;
    std::memcpy(&lo, src, 2);
    std::memcpy(&hi, src + n - 2, 2);
    std::memcpy(dst, &lo, 2);
    std::memcpy(dst + n - 2, &hi, 2);
  } else if (n == 1) {
    dst[0] = src[0];
  }
}

// Same idea, extended to n <= 32 using a pair of 16-byte (SSE) overlapping
// stores for the 17..32 range.
inline void CopyOverlap32(std::string_view sv, char* dst) noexcept {
  const char* src = sv.data();
  std::size_t n = sv.size();
  if (n > 32) {
    std::memcpy(dst, src, n);
    return;
  }
  if (n >= 16) {
    // Two overlapping 16-byte copies via 2x uint64 each (lets the compiler pick
    // movups without us pulling in <immintrin.h>).
    std::uint64_t lo0;
    std::uint64_t lo1;
    std::uint64_t hi0;
    std::uint64_t hi1;
    std::memcpy(&lo0, src, 8);
    std::memcpy(&lo1, src + 8, 8);
    std::memcpy(&hi0, src + n - 16, 8);
    std::memcpy(&hi1, src + n - 8, 8);
    std::memcpy(dst, &lo0, 8);
    std::memcpy(dst + 8, &lo1, 8);
    std::memcpy(dst + n - 16, &hi0, 8);
    std::memcpy(dst + n - 8, &hi1, 8);
  } else if (n >= 8) {
    std::uint64_t lo;
    std::uint64_t hi;
    std::memcpy(&lo, src, 8);
    std::memcpy(&hi, src + n - 8, 8);
    std::memcpy(dst, &lo, 8);
    std::memcpy(dst + n - 8, &hi, 8);
  } else if (n >= 4) {
    std::uint32_t lo;
    std::uint32_t hi;
    std::memcpy(&lo, src, 4);
    std::memcpy(&hi, src + n - 4, 4);
    std::memcpy(dst, &lo, 4);
    std::memcpy(dst + n - 4, &hi, 4);
  } else if (n >= 2) {
    std::uint16_t lo;
    std::uint16_t hi;
    std::memcpy(&lo, src, 2);
    std::memcpy(&hi, src + n - 2, 2);
    std::memcpy(dst, &lo, 2);
    std::memcpy(dst + n - 2, &hi, 2);
  } else if (n == 1) {
    dst[0] = src[0];
  }
}

// The library implementation, so a header change is reflected here directly.
inline void AeronetCopy(std::string_view sv, char* dst) noexcept { aeronet::Copy(sv, dst); }

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

constexpr std::size_t kDstSize = 1U << 16U;  // 64 KiB working set, stays in L2
constexpr int kCopiesPerIter = 4096;

std::vector<char> MakeDst() { return std::vector<char>(kDstSize + 64U); }

// A realistic spread of HTTP fragment lengths: methods (3-7), header names
// (4-20), header values (5-40), with the small end dominating.
std::vector<std::string> MakeHttpishStrings() {
  constexpr std::size_t kCount = 20'000;
  std::vector<std::string> out;
  out.reserve(kCount);

  std::mt19937_64 rng(0xC0FFEE);
  // 50% header-name-ish, 35% short value, 15% method-ish.
  std::discrete_distribution<int> kind({15, 50, 35});
  std::normal_distribution<double> nameLen(11.0, 4.0);  // ~ "Content-Type"
  std::normal_distribution<double> valLen(18.0, 10.0);  // values, wider spread
  std::uniform_int_distribution<int> methodLen(3, 7);   // GET .. OPTIONS
  std::uniform_int_distribution<int> chr('a', 'z');

  auto clamp = [](double v, int lo, int hi) {
    int i = static_cast<int>(v);
    return i < lo ? lo : (i > hi ? hi : i);
  };

  for (std::size_t i = 0; i < kCount; ++i) {
    int len = 0;
    switch (kind(rng)) {
      case 0:
        len = methodLen(rng);
        break;
      case 1:
        len = clamp(nameLen(rng), 3, 24);
        break;
      default:
        len = clamp(valLen(rng), 1, 40);
        break;
    }
    std::string s(static_cast<std::size_t>(len), 'x');
    for (auto& c : s) {
      c = static_cast<char>(chr(rng));
    }
    out.emplace_back(std::move(s));
  }
  return out;
}

const std::vector<std::string> kHttpStrings = MakeHttpishStrings();

// ---------------------------------------------------------------------------
// Benchmarks
// ---------------------------------------------------------------------------

template <auto CopyFn>
void BM_FixedSize(benchmark::State& state) {
  const auto n = static_cast<std::size_t>(state.range(0));  // runtime-opaque size
  const std::string src(n, 'x');
  const std::string_view sv(src);
  auto dst = MakeDst();
  char* const begin = dst.data();
  char* const limit = begin + kDstSize;

  for (auto _ : state) {
    char* d = begin;
    for (int i = 0; i < kCopiesPerIter; ++i) {
      CopyFn(sv, d);
      d += n;
      if (d + 64 > limit) {
        d = begin;
      }
    }
    benchmark::DoNotOptimize(dst.data());
    benchmark::ClobberMemory();
  }

  state.SetItemsProcessed(state.iterations() * kCopiesPerIter);
  state.SetBytesProcessed(state.iterations() * kCopiesPerIter * static_cast<int64_t>(n));
}

template <auto CopyFn>
void BM_MixedHttp(benchmark::State& state) {
  auto dst = MakeDst();
  char* const begin = dst.data();
  char* const limit = begin + kDstSize;

  std::int64_t bytes = 0;
  for (auto _ : state) {
    char* ptr = begin;
    for (const auto& str : kHttpStrings) {
      CopyFn(std::string_view(str), ptr);
      ptr += str.size();
      if (ptr + 64 > limit) {
        ptr = begin;
      }
    }
    benchmark::DoNotOptimize(dst.data());
    benchmark::ClobberMemory();
  }
  for (const auto& str : kHttpStrings) {
    bytes += static_cast<std::int64_t>(str.size());
  }

  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(kHttpStrings.size()));
  state.SetBytesProcessed(state.iterations() * bytes);
}

}  // namespace

#define AERONET_FIXED_SIZES Arg(4)->Arg(8)->Arg(12)->Arg(24)->Arg(32)->Arg(64)->Arg(96)->Arg(256)->Arg(512)->Arg(1024)

BENCHMARK_TEMPLATE(BM_FixedSize, CopyMemcpy)->AERONET_FIXED_SIZES;
BENCHMARK_TEMPLATE(BM_FixedSize, CopyOverlap16)->AERONET_FIXED_SIZES;
BENCHMARK_TEMPLATE(BM_FixedSize, CopyOverlap32)->AERONET_FIXED_SIZES;
BENCHMARK_TEMPLATE(BM_FixedSize, AeronetCopy)->AERONET_FIXED_SIZES;

BENCHMARK_TEMPLATE(BM_MixedHttp, CopyMemcpy);
BENCHMARK_TEMPLATE(BM_MixedHttp, CopyOverlap16);
BENCHMARK_TEMPLATE(BM_MixedHttp, CopyOverlap32);
BENCHMARK_TEMPLATE(BM_MixedHttp, AeronetCopy);

BENCHMARK_MAIN();
