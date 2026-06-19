#include <benchmark/benchmark.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "aeronet/city-hash.hpp"
#include "aeronet/flat-hash-map.hpp"
#include "aeronet/string-equal-ignore-case.hpp"
#include "aeronet/tolower-str.hpp"
#include "aeronet/toupperlower.hpp"

using namespace aeronet;

namespace {

// Frozen copy of the pre-change scalar implementation, for a direct baseline.
bool CIEqualScalar(std::string_view lhs, std::string_view rhs) noexcept {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  const char* pLhs = lhs.data();
  const char* pRhs = rhs.data();
  const char* end = pLhs + lhs.size();
  for (; pLhs != end; ++pLhs, ++pRhs) {
    if (tolower(*pLhs) != tolower(*pRhs)) {
      return false;
    }
  }
  return true;
}

// SWAR prototype: lowercase + compare 8 bytes at a time via AsciiLowerMask, overlapping tail; scalar for <8.
// always_inline so the old-vs-new algorithm comparison is measured inlined (as it is at real call sites),
// not dominated by the per-call overhead of an opaque function-pointer call.
[[gnu::always_inline]] inline bool CIEqualSwar(std::string_view lhs, std::string_view rhs) noexcept {
  const std::size_t len = lhs.size();
  if (len != rhs.size()) {
    return false;
  }
  const char* pLhs = lhs.data();
  const char* pRhs = rhs.data();
  if (len >= 8) {
    std::size_t pos = 0;
    for (; pos + 8 <= len; pos += 8) {
      std::uint64_t a;
      std::uint64_t b;
      std::memcpy(&a, pLhs + pos, sizeof(a));
      std::memcpy(&b, pRhs + pos, sizeof(b));
      if (AsciiLowerMask(a) != AsciiLowerMask(b)) {
        return false;
      }
    }
    if (pos != len) {
      std::uint64_t a;
      std::uint64_t b;
      std::memcpy(&a, pLhs + len - 8, sizeof(a));
      std::memcpy(&b, pRhs + len - 8, sizeof(b));
      if (AsciiLowerMask(a) != AsciiLowerMask(b)) {
        return false;
      }
    }
    return true;
  }
  for (std::size_t i = 0; i < len; ++i) {
    if (tolower(pLhs[i]) != tolower(pRhs[i])) {
      return false;
    }
  }
  return true;
}

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
// SSE2 (128-bit) lowercase of 16 bytes, mirroring the signed-compare trick of AsciiLowerMask but 16-wide.
inline __m128i SseAsciiLower(__m128i input) {
  const auto aMinus1 = _mm_set1_epi8(static_cast<char>('A' - 1));
  const auto zPlus1 = _mm_set1_epi8(static_cast<char>('Z' + 1));
  const auto geA = _mm_cmpgt_epi8(input, aMinus1);
  const auto ltZ = _mm_cmpgt_epi8(zPlus1, input);
  const auto isUpper = _mm_and_si128(geA, ltZ);
  const auto lowerBit = _mm_and_si128(isUpper, _mm_set1_epi8(0x20));
  return _mm_or_si128(input, lowerBit);
}

// Lowercase + compare 16 bytes at a time entirely in the XMM domain: one unaligned load per side, lowercase,
// _mm_cmpeq_epi8 + movemask. Avoids the GPR<->XMM round trips the 8-byte SWAR path pays per chunk.
inline bool SseLower16Equal(const char* a, const char* b) {
  const auto va = _mm_loadu_si128(reinterpret_cast<const __m128i*>(a));
  const auto vb = _mm_loadu_si128(reinterpret_cast<const __m128i*>(b));
  const auto eq = _mm_cmpeq_epi8(SseAsciiLower(va), SseAsciiLower(vb));
  return _mm_movemask_epi8(eq) == 0xFFFF;
}

// SSE prototype: mirrors the shipped CaseInsensitiveEqual exactly -- 16 bytes at a time (overlapping tail),
// 8-byte SWAR for [8,16), scalar for <8. always_inline for the same reason as CIEqualSwar.
[[gnu::always_inline]] inline bool CIEqualSse(std::string_view lhs, std::string_view rhs) noexcept {
  const std::size_t len = lhs.size();
  if (len != rhs.size()) {
    return false;
  }
  const char* pLhs = lhs.data();
  const char* pRhs = rhs.data();
  if (len >= 16) {
    std::size_t pos = 0;
    for (; pos + 16 <= len; pos += 16) {
      if (!SseLower16Equal(pLhs + pos, pRhs + pos)) {
        return false;
      }
    }
    if (pos != len) {
      return SseLower16Equal(pLhs + len - 16, pRhs + len - 16);
    }
    return true;
  }
  if (len >= 8) {
    std::size_t pos = 0;
    for (; pos + 8 <= len; pos += 8) {
      std::uint64_t a;
      std::uint64_t b;
      std::memcpy(&a, pLhs + pos, sizeof(a));
      std::memcpy(&b, pRhs + pos, sizeof(b));
      if (AsciiLowerMask(a) != AsciiLowerMask(b)) {
        return false;
      }
    }
    if (pos != len) {
      std::uint64_t a;
      std::uint64_t b;
      std::memcpy(&a, pLhs + len - 8, sizeof(a));
      std::memcpy(&b, pRhs + len - 8, sizeof(b));
      return AsciiLowerMask(a) == AsciiLowerMask(b);
    }
    return true;
  }
  for (std::size_t i = 0; i < len; ++i) {
    if (tolower(pLhs[i]) != tolower(pRhs[i])) {
      return false;
    }
  }
  return true;
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("avx2")))
#endif
#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("avx2")))
#endif
inline __m256i Avx2AsciiLower(__m256i in, __m256i aMinus1, __m256i zPlus1, __m256i lowerMask) {
  const auto isUpper = _mm256_and_si256(_mm256_cmpgt_epi8(in, aMinus1), _mm256_cmpgt_epi8(zPlus1, in));
  return _mm256_or_si256(in, _mm256_and_si256(isUpper, lowerMask));
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("avx2")))
#endif
inline bool Avx2Lower32Equal(const char* a, const char* b) {
  const auto va = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a));
  const auto vb = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b));
  const auto aMinus1 = _mm256_set1_epi8(static_cast<char>('A' - 1));
  const auto zPlus1 = _mm256_set1_epi8(static_cast<char>('Z' + 1));
  const auto lowerMask = _mm256_set1_epi8(0x20);
  const auto eq = _mm256_cmpeq_epi8(Avx2AsciiLower(va, aMinus1, zPlus1, lowerMask),
                                    Avx2AsciiLower(vb, aMinus1, zPlus1, lowerMask));
  return static_cast<unsigned>(_mm256_movemask_epi8(eq)) == 0xFFFFFFFFU;
}

// AVX2 prototype: 32 bytes at a time when the CPU supports it (runtime dispatch, since the build is generic
// x86-64), falling back to the SSE path for shorter inputs or non-AVX2 CPUs. The 32-byte step itself stays an
// out-of-line target("avx2") call (it cannot be inlined into generic code) -- that is the structural cost the
// benchmark exposes; everything else is inlined like the other candidates for a fair comparison.
[[gnu::always_inline]] inline bool CIEqualAvx2(std::string_view lhs, std::string_view rhs) noexcept {
  static const bool kHasAvx2 = HasAvx2ForToLower();
  const std::size_t len = lhs.size();
  if (len != rhs.size()) {
    return false;
  }
  if (len >= 32 && kHasAvx2) {
    const char* pLhs = lhs.data();
    const char* pRhs = rhs.data();
    std::size_t pos = 0;
    for (; pos + 32 <= len; pos += 32) {
      if (!Avx2Lower32Equal(pLhs + pos, pRhs + pos)) {
        return false;
      }
    }
    if (pos != len) {
      return Avx2Lower32Equal(pLhs + len - 32, pRhs + len - 32);
    }
    return true;
  }
  return CIEqualSse(lhs, rhs);
}
#endif  // x86

bool CIEqualLibrary(std::string_view lhs, std::string_view rhs) noexcept { return CaseInsensitiveEqual(lhs, rhs); }

struct CaseInsensitiveHashBoostStyle {
  std::size_t operator()(std::string_view str) const noexcept {
    std::size_t hash = 0;
    for (unsigned char c : str) {
      hash ^= static_cast<std::size_t>(tolower(c)) + static_cast<std::size_t>(0x9e3779b97f4a7c15ULL) + (hash << 6) +
              (hash >> 2);
    }
    return hash;
  }
};

struct CaseInsensitiveHashFnv1Style {
  std::size_t operator()(std::string_view str) const noexcept {
    std::size_t hash = 14695981039346656037ULL;
    for (char c : str) {
      hash ^= tolower(c);
      hash *= 1099511628211ULL;
    }
    return hash;
  }
};

std::vector<std::string> GenerateTestStrings() {
  constexpr std::size_t kCount = 100'000;
  constexpr std::size_t kMinLen = 4;
  constexpr std::size_t kMaxLen = 96;

  std::vector<std::string> strings;
  strings.reserve(kCount);

  std::mt19937_64 rng(0xC0FFEE);
  std::normal_distribution<double> length_dist(16.0, 8.0);
  std::uniform_int_distribution<int> char_dist('a', 'z');
  std::bernoulli_distribution upper_dist(0.3);  // mix upper/lowercase

  for (std::size_t i = 0; i < kCount; ++i) {
    std::size_t len;
    do {
      len = static_cast<std::size_t>(length_dist(rng));
    } while (len < kMinLen || len > kMaxLen);

    std::string s;
    s.resize(len);

    for (std::size_t j = 0; j < len; ++j) {
      char c = static_cast<char>(char_dist(rng));
      if (upper_dist(rng)) {
        c = static_cast<char>(c - 'a' + 'A');
      }
      s[j] = c;
    }

    strings.emplace_back(std::move(s));
  }

  return strings;
}

const auto kStorage = GenerateTestStrings();

template <typename Hash>
void BuildMap(const std::vector<std::string>& storage, auto& map) {
  map.reserve(storage.size());
  // map.max_load_factor(0.7f);

  for (const auto& s : storage) {
    map.emplace(s, s);
  }
}

}  // namespace

// ------------------------------------------------------------
// Benchmarks
// ------------------------------------------------------------

static void BM_Hash_CI_Boost(benchmark::State& state) {
  CaseInsensitiveHashBoostStyle hasher;

  for (auto _ : state) {
    for (std::string_view s : kStorage) {
      benchmark::DoNotOptimize(hasher(s));
    }
  }

  state.SetItemsProcessed(state.iterations() * kStorage.size());
}

static void BM_Hash_CI_FNV1a(benchmark::State& state) {
  CaseInsensitiveHashFnv1Style hasher;

  for (auto _ : state) {
    for (std::string_view s : kStorage) {
      benchmark::DoNotOptimize(hasher(s));
    }
  }

  state.SetItemsProcessed(state.iterations() * kStorage.size());
}

static void BM_Hash_City(benchmark::State& state) {
  CityHash hasher;

  for (auto _ : state) {
    for (std::string_view s : kStorage) {
      benchmark::DoNotOptimize(hasher(s));
    }
  }

  state.SetItemsProcessed(state.iterations() * kStorage.size());
}

static void BM_UnorderedMap_Find_CI_Boost(benchmark::State& state) {
  std::unordered_map<std::string_view, std::string_view, CaseInsensitiveHashBoostStyle, CaseInsensitiveEqualFunc> map;
  BuildMap<CaseInsensitiveHashBoostStyle>(kStorage, map);

  for (auto _ : state) {
    for (std::string_view s : kStorage) {
      benchmark::DoNotOptimize(map.find(s));
    }
  }

  state.SetItemsProcessed(state.iterations() * kStorage.size());
}

static void BM_UnorderedMap_Find_CI_FNV1a(benchmark::State& state) {
  std::unordered_map<std::string_view, std::string_view, CaseInsensitiveHashFnv1Style, CaseInsensitiveEqualFunc> map;
  BuildMap<CaseInsensitiveHashFnv1Style>(kStorage, map);

  for (auto _ : state) {
    for (std::string_view s : kStorage) {
      benchmark::DoNotOptimize(map.find(s));
    }
  }

  state.SetItemsProcessed(state.iterations() * kStorage.size());
}
static void BM_UnorderedMap_Find_City(benchmark::State& state) {
  std::unordered_map<std::string_view, std::string_view, CityHash, CaseInsensitiveEqualFunc> map;
  BuildMap<CityHash>(kStorage, map);

  for (auto _ : state) {
    for (std::string_view s : kStorage) {
      benchmark::DoNotOptimize(map.find(s));
    }
  }

  state.SetItemsProcessed(state.iterations() * kStorage.size());
}

static void BM_FlatHashMap_Find_CI_Boost(benchmark::State& state) {
  flat_hash_map<std::string_view, std::string_view, CaseInsensitiveHashBoostStyle, CaseInsensitiveEqualFunc> map;
  BuildMap<CaseInsensitiveHashBoostStyle>(kStorage, map);

  for (auto _ : state) {
    for (std::string_view s : kStorage) {
      benchmark::DoNotOptimize(map.find(s));
    }
  }

  state.SetItemsProcessed(state.iterations() * kStorage.size());
}

static void BM_FlatHashMap_Find_CI_FNV1a(benchmark::State& state) {
  flat_hash_map<std::string_view, std::string_view, CaseInsensitiveHashFnv1Style, CaseInsensitiveEqualFunc> map;
  BuildMap<CaseInsensitiveHashFnv1Style>(kStorage, map);

  for (auto _ : state) {
    for (std::string_view s : kStorage) {
      benchmark::DoNotOptimize(map.find(s));
    }
  }

  state.SetItemsProcessed(state.iterations() * kStorage.size());
}

static void BM_FlatHashMap_Find_City(benchmark::State& state) {
  flat_hash_map<std::string_view, std::string_view, CityHash> map;
  BuildMap<CityHash>(kStorage, map);

  for (auto _ : state) {
    for (std::string_view s : kStorage) {
      benchmark::DoNotOptimize(map.find(s));
    }
  }

  state.SetItemsProcessed(state.iterations() * kStorage.size());
}

static void BM_FlatHashMap_Find_Sv(benchmark::State& state) {
  flat_hash_map<std::string_view, std::string_view, std::hash<std::string_view>> map;
  BuildMap<std::hash<std::string_view>>(kStorage, map);

  for (auto _ : state) {
    for (std::string_view s : kStorage) {
      benchmark::DoNotOptimize(map.find(s));
    }
  }

  state.SetItemsProcessed(state.iterations() * kStorage.size());
}

// ------------------------------------------------------------

BENCHMARK(BM_Hash_CI_Boost);
BENCHMARK(BM_Hash_CI_FNV1a);
BENCHMARK(BM_Hash_City);

BENCHMARK(BM_UnorderedMap_Find_CI_Boost);
BENCHMARK(BM_UnorderedMap_Find_CI_FNV1a);
BENCHMARK(BM_UnorderedMap_Find_City);

BENCHMARK(BM_FlatHashMap_Find_CI_Boost);
BENCHMARK(BM_FlatHashMap_Find_CI_FNV1a);
BENCHMARK(BM_FlatHashMap_Find_City);
BENCHMARK(BM_FlatHashMap_Find_Sv);

// ------------------------------------------------------------
// CaseInsensitiveEqual: scalar baseline vs SWAR
// ------------------------------------------------------------

namespace {

// Two case-insensitively-equal strings of length n, with alternating case so the lowercasing actually matters.
std::pair<std::string, std::string> MakeEqualMixedCase(std::size_t len) {
  std::string lhs(len, 'x');
  std::string rhs(len, 'x');
  std::mt19937_64 rng(0xABCDEF + len);
  std::uniform_int_distribution<int> chr('a', 'z');
  for (std::size_t i = 0; i < len; ++i) {
    const char lower = static_cast<char>(chr(rng));
    lhs[i] = lower;
    rhs[i] = (i % 2 == 0) ? lower : static_cast<char>(lower - 'a' + 'A');
  }
  return {lhs, rhs};
}

const std::vector<std::string> kHeaderNames = {
    "Host",          "Date",         "ETag",           "Vary",
    "Allow",         "Accept",       "Cookie",         "Server",
    "Expect",        "Origin",       "Connection",     "Content-Type",
    "User-Agent",    "Content-Length", "Accept-Encoding", "Cache-Control",
    "Authorization", "Content-Encoding", "Transfer-Encoding", "If-None-Match",
    "Last-Modified", "Set-Cookie",   "Location",       "Referer",
    "Upgrade",       "Pragma",       "Age",            "Range",
    "TE",            "Via"};

}  // namespace

// Best case for SWAR: equal strings (the actual-match path of a header lookup).
template <auto Fn>
void BM_CIEqual_Equal(benchmark::State& state) {
  auto [lhs, rhs] = MakeEqualMixedCase(static_cast<std::size_t>(state.range(0)));
  std::string_view svl(lhs);
  std::string_view svr(rhs);
  bool sink = false;
  for (auto _ : state) {
    benchmark::DoNotOptimize(svl);
    benchmark::DoNotOptimize(svr);
    sink ^= Fn(svl, svr);
    benchmark::DoNotOptimize(sink);
  }
  state.SetItemsProcessed(state.iterations());
}

// Adversarial for SWAR: same length, differ at byte 0 (scalar can bail after one byte).
template <auto Fn>
void BM_CIEqual_DiffFirst(benchmark::State& state) {
  auto [lhs, rhs] = MakeEqualMixedCase(static_cast<std::size_t>(state.range(0)));
  rhs[0] = static_cast<char>(rhs[0] ^ 0x01);  // not a case-only difference
  std::string_view svl(lhs);
  std::string_view svr(rhs);
  bool sink = false;
  for (auto _ : state) {
    benchmark::DoNotOptimize(svl);
    benchmark::DoNotOptimize(svr);
    sink ^= Fn(svl, svr);
    benchmark::DoNotOptimize(sink);
  }
  state.SetItemsProcessed(state.iterations());
}

// Same length, differ only at the last byte (scalar must scan the whole string).
template <auto Fn>
void BM_CIEqual_DiffLast(benchmark::State& state) {
  auto [lhs, rhs] = MakeEqualMixedCase(static_cast<std::size_t>(state.range(0)));
  rhs[rhs.size() - 1] = static_cast<char>(rhs[rhs.size() - 1] ^ 0x01);
  std::string_view svl(lhs);
  std::string_view svr(rhs);
  bool sink = false;
  for (auto _ : state) {
    benchmark::DoNotOptimize(svl);
    benchmark::DoNotOptimize(svr);
    sink ^= Fn(svl, svr);
    benchmark::DoNotOptimize(sink);
  }
  state.SetItemsProcessed(state.iterations());
}

// Realistic: classify each incoming (case-flipped) header name by linear scan over the known set. Mixes the
// size fast-reject, same-length early mismatches, and the eventual case-insensitive match.
template <auto Fn>
void BM_CIEqual_HeaderClassify(benchmark::State& state) {
  std::vector<std::string> incoming;
  incoming.reserve(kHeaderNames.size());
  for (const auto& name : kHeaderNames) {
    std::string flipped = name;
    for (auto& chr : flipped) {
      chr = static_cast<char>(toupper(static_cast<unsigned char>(chr)));
    }
    incoming.push_back(std::move(flipped));
  }
  std::size_t matches = 0;
  for (auto _ : state) {
    for (const auto& in : incoming) {
      for (const auto& known : kHeaderNames) {
        if (Fn(std::string_view(in), std::string_view(known))) {
          ++matches;
          break;
        }
      }
    }
    benchmark::DoNotOptimize(matches);
  }
  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(incoming.size()));
}

#define AERONET_CIEQ_LENS Arg(3)->Arg(4)->Arg(8)->Arg(12)->Arg(16)->Arg(20)->Arg(32)->Arg(48)->Arg(64)->Arg(96)
#define AERONET_CIEQ_DIFF_LENS Arg(8)->Arg(16)->Arg(32)->Arg(64)

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#define AERONET_CIEQ_WIDE(MACRO, BM) \
  BENCHMARK_TEMPLATE(BM, CIEqualSse)->MACRO; \
  BENCHMARK_TEMPLATE(BM, CIEqualAvx2)->MACRO;
#else
#define AERONET_CIEQ_WIDE(MACRO, BM)
#endif

BENCHMARK_TEMPLATE(BM_CIEqual_Equal, CIEqualScalar)->AERONET_CIEQ_LENS;
BENCHMARK_TEMPLATE(BM_CIEqual_Equal, CIEqualSwar)->AERONET_CIEQ_LENS;
AERONET_CIEQ_WIDE(AERONET_CIEQ_LENS, BM_CIEqual_Equal)
BENCHMARK_TEMPLATE(BM_CIEqual_Equal, CIEqualLibrary)->AERONET_CIEQ_LENS;

BENCHMARK_TEMPLATE(BM_CIEqual_DiffFirst, CIEqualScalar)->AERONET_CIEQ_DIFF_LENS;
BENCHMARK_TEMPLATE(BM_CIEqual_DiffFirst, CIEqualSwar)->AERONET_CIEQ_DIFF_LENS;
AERONET_CIEQ_WIDE(AERONET_CIEQ_DIFF_LENS, BM_CIEqual_DiffFirst)
BENCHMARK_TEMPLATE(BM_CIEqual_DiffFirst, CIEqualLibrary)->AERONET_CIEQ_DIFF_LENS;

BENCHMARK_TEMPLATE(BM_CIEqual_DiffLast, CIEqualScalar)->AERONET_CIEQ_DIFF_LENS;
BENCHMARK_TEMPLATE(BM_CIEqual_DiffLast, CIEqualSwar)->AERONET_CIEQ_DIFF_LENS;
AERONET_CIEQ_WIDE(AERONET_CIEQ_DIFF_LENS, BM_CIEqual_DiffLast)
BENCHMARK_TEMPLATE(BM_CIEqual_DiffLast, CIEqualLibrary)->AERONET_CIEQ_DIFF_LENS;

BENCHMARK_TEMPLATE(BM_CIEqual_HeaderClassify, CIEqualScalar);
BENCHMARK_TEMPLATE(BM_CIEqual_HeaderClassify, CIEqualSwar);
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
BENCHMARK_TEMPLATE(BM_CIEqual_HeaderClassify, CIEqualSse);
BENCHMARK_TEMPLATE(BM_CIEqual_HeaderClassify, CIEqualAvx2);
#endif
BENCHMARK_TEMPLATE(BM_CIEqual_HeaderClassify, CIEqualLibrary);

BENCHMARK_MAIN();
