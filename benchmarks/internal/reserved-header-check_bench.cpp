// Benchmark: IsReservedOrForbiddenRequestHeader (binary search over a
// lowercased stack buffer) vs. a linear scan using CaseInsensitiveEqual
// (SIMD-accelerated case-insensitive compare with early length/byte exit).
#include <benchmark/benchmark.h>

#include <algorithm>
#include <array>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "aeronet/http-constants.hpp"
#include "aeronet/string-equal-ignore-case.hpp"
#include "aeronet/tolower-str.hpp"

namespace aeronet::http {

namespace {

template <auto& kReservedOrderedLowerCaseHeaders>
constexpr bool IsReservedHeaderImpl(std::string_view name) noexcept {
  static_assert(std::ranges::is_sorted(kReservedOrderedLowerCaseHeaders));

  static constexpr auto kMaxLenReserved =
      std::ranges::max_element(kReservedOrderedLowerCaseHeaders, {}, [](std::string_view sv) {
        return sv.size();
      })->size();
  if (name.size() > kMaxLenReserved) {
    return false;
  }

  char lowerCaseName[kMaxLenReserved];
  tolower_n(name.data(), name.size(), lowerCaseName);
  return std::ranges::binary_search(kReservedOrderedLowerCaseHeaders, std::string_view{lowerCaseName, name.size()});
}

constexpr std::string_view kHeaders[] = {"authorization",
                                         "cache-control",
                                         "content-encoding",
                                         "content-length",
                                         "content-range",
                                         "content-type",
                                         "cookie",
                                         "expect",
                                         "expires",
                                         "host",
                                         "if-match",
                                         "if-modified-since",
                                         "if-none-match",
                                         "if-unmodified-since",
                                         "pragma",
                                         "range",
                                         "set-cookie",
                                         "te",
                                         "trailer",
                                         "transfer-encoding",
                                         "vary"};

// Candidate for replacing IsReservedOrForbiddenRequestHeader: linear scan
// using the project's SIMD CaseInsensitiveEqual. CaseInsensitiveEqual
// rejects on a length mismatch in O(1) before touching any bytes, so most
// candidates in this 10-entry table are rejected essentially for free;
// only same-length candidates pay for an actual byte comparison.
constexpr bool IsReservedHeaderLinear(std::string_view name) noexcept {
  return std::ranges::any_of(kHeaders,
                             [name](std::string_view candidate) { return CaseInsensitiveEqual(name, candidate); });
}

// Same as IsReservedResponseHeader but for request headers.
constexpr bool IsReservedHeaderBinary(std::string_view name) noexcept { return IsReservedHeaderImpl<kHeaders>(name); }

// ---------------------------------------------------------------------
// Scenario data. All strings live in runtime containers (not string
// literals fed straight to the function) so the compiler cannot constant
// fold the calls away, and are shuffled with a fixed seed so both
// implementations see byte-for-byte the same access pattern.
// ---------------------------------------------------------------------

std::vector<std::string> Shuffled(std::vector<std::string> vec, unsigned seed) {
  std::mt19937 rng(seed);
  std::ranges::shuffle(vec, rng);
  return vec;
}

// Realistic mix: the 10 reserved/forbidden headers plus a broad sample of
// common request headers that should NOT match (short, medium, and a
// couple of long custom ones) -- roughly what a real request looks like.
const std::vector<std::string>& RealisticMix() {
  static const std::vector<std::string> kSample = Shuffled(
      {
          // reserved / forbidden (10)
          "Content-Length",
          "Content-Type",
          "Host",
          "Expect",
          "Transfer-Encoding",
          "Vary",
          "TE",
          "Trailer",
          "Expires",
          "Cookie",
          // common, legitimate, non-reserved (the hot/common case)
          "Accept",
          "Accept-Encoding",
          "Accept-Language",
          "Abcded",
          "Betyu",
          "Server",
          "If-Something",
          "If-Modified-Sincg",
          "Origin",
          "Referer",
          "User-Agent",
          "X-Requested-With",
          "X-Forwarded-For",
          "X-Forwarded-Proto",
          "X-Correlation-Id",
          "X-Request-Id",
          "Sec-Fetch-Site",
          "Sec-Fetch-Mode",
          "Sec-Fetch-Dest",
          "Sec-Ch-Ua",
          "Sec-Ch-Ua-Platform",
          "X-Pragma",
          "DNT",
          "X-Custom-Header",
          "X-My-Custom-Header",

          // long custom headers (>16 bytes), exercises SIMD tail path
          "X-Custom-Application-Trace-Id",
          "X-My-Very-Long-Custom-Header-Name",
      },
      /*seed=*/42);
  return kSample;
}

// Only non-reserved headers: the common path in production, where the
// check always has to fail (every candidate rejected).
const std::vector<std::string>& NeverReserved() {
  static const std::vector<std::string> kSample = Shuffled(
      {
          "Accept",
          "Accept-Encoding",
          "Accept-Language",
          "Abcded",
          "Betyu",
          "Server",
          "If-Something",
          "If-Modified-Sincg",
          "Origin",
          "Referer",
          "User-Agent",
          "X-Requested-With",
          "X-Forwarded-For",
          "X-Forwarded-Proto",
          "X-Correlation-Id",
          "X-Request-Id",
          "Sec-Fetch-Site",
          "Sec-Fetch-Mode",
          "Sec-Fetch-Dest",
          "Sec-Ch-Ua",
          "Sec-Ch-Ua-Platform",
          "X-Pragma",
      },
      /*seed=*/7);
  return kSample;
}

// Only reserved headers: worst case for "must confirm a match", cycles
// through all 10 entries of the table.
const std::vector<std::string>& AlwaysReserved() {
  static const std::vector<std::string> kSample =
      Shuffled({"Content-Length", "Content-Type", "Host", "if-none-match", "Transfer-Encoding", "pragma", "TE",
                "Trailer", "authorization", "Set-Cookie"},
               /*seed=*/13);
  return kSample;
}

// Near-miss set: same length as a reserved header, differs only in the
// last byte (e.g. "transfer-encodinX"). Forces a near-complete byte
// comparison before rejecting -- the real worst case for the *shape* of
// the comparison itself, as opposed to a length-based fast reject.
const std::vector<std::string>& NearMiss() {
  static const std::vector<std::string> kSample = [] {
    std::vector<std::string> v;
    for (std::string_view h : {"Content-Length", "Content-Type", "Host", "if-none-match", "Transfer-Encoding", "pragma",
                               "TE", "Trailer", "authorization", "Set-Cookie"}) {
      std::string str(h);
      str.back() = (str.back() == 'x') ? 'y' : 'x';  // flip last byte, keep length/case pattern
      v.push_back(std::move(str));
    }
    return Shuffled(std::move(v), /*seed=*/99);
  }();
  return kSample;
}

template <auto Fn>
void RunOver(benchmark::State& state, const std::vector<std::string>& data) {
  std::vector<std::string_view> views(data.begin(), data.end());
  std::size_t idx = 0;
  for (auto _ : state) {
    std::string_view key = views[idx];
    // Non-const-ref DoNotOptimize: the const-ref overload is only a
    // read-barrier and, as of this benchmark version, is documented as
    // insufficient to stop the compiler reusing a previously-derived
    // result -- see the ByLength benchmarks below for a case where that
    // distinction is the difference between a real measurement and 0ns
    // of hoisted-away nothing. Non-const forces a read-modify-write
    // clobber, which is the safe default here even though indexing
    // through `views` already gives the compiler a harder time.
    benchmark::DoNotOptimize(key);
    bool result = Fn(key);
    benchmark::DoNotOptimize(result);
    idx = (idx + 1 == views.size()) ? 0 : idx + 1;
  }
  state.SetItemsProcessed(state.iterations());
}
}  // namespace

// --- Realistic mix (both reserved and non-reserved headers) ---
static void BM_Binary_RealisticMix(benchmark::State& s) { RunOver<IsReservedHeaderBinary>(s, RealisticMix()); }
static void BM_Linear_RealisticMix(benchmark::State& s) { RunOver<IsReservedHeaderLinear>(s, RealisticMix()); }
BENCHMARK(BM_Binary_RealisticMix);
BENCHMARK(BM_Linear_RealisticMix);

// --- Hot path: headers that are never reserved (always rejected) ---
static void BM_Binary_NeverReserved(benchmark::State& s) { RunOver<IsReservedHeaderBinary>(s, NeverReserved()); }
static void BM_Linear_NeverReserved(benchmark::State& s) { RunOver<IsReservedHeaderLinear>(s, NeverReserved()); }
BENCHMARK(BM_Binary_NeverReserved);
BENCHMARK(BM_Linear_NeverReserved);

// --- Rare path: headers that always match a reserved entry ---
static void BM_Binary_AlwaysReserved(benchmark::State& s) { RunOver<IsReservedHeaderBinary>(s, AlwaysReserved()); }
static void BM_Linear_AlwaysReserved(benchmark::State& s) { RunOver<IsReservedHeaderLinear>(s, AlwaysReserved()); }
BENCHMARK(BM_Binary_AlwaysReserved);
BENCHMARK(BM_Linear_AlwaysReserved);

// --- Worst case shape: same length, differs only in the last byte ---
static void BM_Binary_NearMiss(benchmark::State& s) { RunOver<IsReservedHeaderBinary>(s, NearMiss()); }
static void BM_Linear_NearMiss(benchmark::State& s) { RunOver<IsReservedHeaderLinear>(s, NearMiss()); }
BENCHMARK(BM_Binary_NearMiss);
BENCHMARK(BM_Linear_NearMiss);

// --- Length sweep: guaranteed-mismatch synthetic keys at fixed lengths,
// to see how each approach scales as header-name length grows (this is
// where the SIMD 8/16-byte blocks in CaseInsensitiveEqual should start
// to show a widening gap vs. the scalar tolower_n + binary_search path).
static void BM_Binary_ByLength(benchmark::State& state) {
  const auto len = static_cast<std::size_t>(state.range(0));
  const std::string key(len, 'z');  // mismatches every candidate
  std::string_view sv = key;
  for (auto _ : state) {
    // The non-const-ref DoNotOptimize overload stops the compiler from
    // proving this call is loop-invariant and folding/hoisting it away
    // at compile time (the const-ref overload is documented as
    // insufficient for that). It does NOT stop a superscalar/OoO CPU
    // from overlapping the (genuinely re-executed) iterations at
    // runtime when there's no true data dependency from one iteration's
    // output to the next's input, as is the case here. In practice this
    // means: any candidate length where the function's real work is a
    // single cheap branch (e.g. name.size() > kMaxLenReserved here) can
    // still report numbers well under 1ns -- not because the branch is
    // free, but because the CPU pipelines many back-to-back independent
    // checks. Treat sub-~1ns figures from this sweep as "very fast,
    // exact value not meaningful" rather than a literal single-call
    // latency; a true isolated-latency number would need an artificial
    // dependency chain between iterations, which isn't done here since
    // it wouldn't change the conclusion for a single-comparison branch.
    benchmark::DoNotOptimize(sv);
    bool result = IsReservedHeaderBinary(sv);
    benchmark::DoNotOptimize(result);
  }
}

static void BM_Linear_ByLength(benchmark::State& state) {
  const auto len = static_cast<std::size_t>(state.range(0));
  const std::string key(len, 'z');
  std::string_view sv = key;
  for (auto _ : state) {
    benchmark::DoNotOptimize(sv);  // see BM_Binary_ByLength for why this must be non-const
    bool result = IsReservedHeaderLinear(sv);
    benchmark::DoNotOptimize(result);
  }
}

BENCHMARK(BM_Binary_ByLength)->Arg(2)->Arg(4)->Arg(6)->Arg(8)->Arg(10)->Arg(14)->Arg(18)->Arg(24)->Arg(32);
BENCHMARK(BM_Linear_ByLength)->Arg(2)->Arg(4)->Arg(6)->Arg(8)->Arg(10)->Arg(14)->Arg(18)->Arg(24)->Arg(32);

}  // namespace aeronet::http

BENCHMARK_MAIN();