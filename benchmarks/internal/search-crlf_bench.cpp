// Benchmark SearchCRLF implementations on realistic HTTP/1.1 request bytes.
//
// The parser calls SearchCRLF once for the request line and once per header, with
// the end of the whole input buffer as the search limit. These benchmarks mirror
// that access pattern and compare the current hybrid production implementation,
// the previous std::memchr implementation, and SSE2/AVX2 prototypes. Bounded SSE2
// variants scan only a short prefix before falling back to std::memchr; the request
// corpora decide which prefix helps the actual parser access pattern.

#include <benchmark/benchmark.h>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aeronet/compiler-config.hpp"
#include "aeronet/memory-utils.hpp"
#include "aeronet/search-crlf.hpp"

#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
#include <immintrin.h>
#define AERONET_BENCH_HAS_SSE2 1
#endif

#if (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(__i386__))
#define AERONET_BENCH_HAS_AVX2_TARGET 1
#endif

namespace {

using SearchResult = const char*;

struct Corpus {
  std::vector<std::string> requests;
  std::int64_t totalBytes = 0;
  std::int64_t totalLines = 0;
};

AERONET_ALWAYS_INLINE SearchResult SearchCRLFProduction(const char* begin, const char* end) noexcept {
  return aeronet::SearchCRLF(begin, end);
}

AERONET_ALWAYS_INLINE SearchResult SearchCRLFMemchrRestart(const char* begin, const char* end) noexcept {
  for (; begin != end; ++begin) {
    const void* ptr = std::memchr(begin, '\r', static_cast<std::size_t>(end - begin));
    if (ptr == nullptr) {
      return end;
    }
    begin = static_cast<const char*>(ptr);
    if (begin + 1 < end && begin[1] == '\n') {
      return begin;
    }
  }
  return end;
}

AERONET_ALWAYS_INLINE SearchResult SearchCRLFStringViewFind(const char* begin, const char* end) noexcept {
  const std::string_view haystack(begin, static_cast<std::size_t>(end - begin));
  const auto pos = haystack.find("\r\n", 0, 2);
  return pos == std::string_view::npos ? end : begin + pos;
}

#ifdef AERONET_BENCH_HAS_SSE2
AERONET_ALWAYS_INLINE SearchResult SearchCRLFSse2Until(const char* begin, const char* end,
                                                       const char* sseEnd) noexcept {
  const __m128i cr = _mm_set1_epi8('\r');
  while (sseEnd - begin >= 16) {
    const __m128i chunk = _mm_loadu_si128(reinterpret_cast<const __m128i*>(begin));
    unsigned int mask = static_cast<unsigned int>(_mm_movemask_epi8(_mm_cmpeq_epi8(chunk, cr)));
    while (mask != 0U) {
      const auto offset = std::countr_zero(mask);
      const char* position = begin + offset;
      if (position + 1 < end && position[1] == '\n') {
        return position;
      }
      mask &= mask - 1U;
    }
    begin += 16;
  }
  return SearchCRLFMemchrRestart(begin, end);
}

template <std::size_t SsePrefixBytes>
AERONET_ALWAYS_INLINE SearchResult SearchCRLFSse2ThenMemchr(const char* begin, const char* end) noexcept {
  static_assert(SsePrefixBytes != 0 && SsePrefixBytes % 16 == 0);
  const char* const sseEnd = end - begin < static_cast<std::ptrdiff_t>(SsePrefixBytes) ? end : begin + SsePrefixBytes;
  return SearchCRLFSse2Until(begin, end, sseEnd);
}

AERONET_ALWAYS_INLINE SearchResult SearchCRLFSse2(const char* begin, const char* end) noexcept {
  return SearchCRLFSse2Until(begin, end, end);
}
#endif

#ifdef AERONET_BENCH_HAS_AVX2_TARGET
[[gnu::target("avx2")]] SearchResult SearchCRLFAvx2(const char* begin, const char* end) noexcept {
  const __m256i cr = _mm256_set1_epi8('\r');
  while (end - begin >= 32) {
    const __m256i chunk = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(begin));
    unsigned int mask = static_cast<unsigned int>(_mm256_movemask_epi8(_mm256_cmpeq_epi8(chunk, cr)));
    while (mask != 0U) {
      const auto offset = std::countr_zero(mask);
      const char* position = begin + offset;
      if (position + 1 < end && position[1] == '\n') {
        return position;
      }
      mask &= mask - 1U;
    }
    begin += 32;
  }
  return SearchCRLFMemchrRestart(begin, end);
}

bool HasAvx2() noexcept {
  static const bool hasAvx2 = __builtin_cpu_supports("avx2");
  return hasAvx2;
}
#endif

void AddRequestStats(Corpus& corpus, const std::string& request) {
  corpus.totalBytes += static_cast<std::int64_t>(request.size());
  for (const char ch : request) {
    corpus.totalLines += static_cast<std::int64_t>(ch == '\n');
  }
}

Corpus BuildTypicalCorpus() {
  static constexpr std::string_view kMethods[]{"GET", "GET", "GET", "POST", "PUT", "DELETE"};
  static constexpr std::string_view kPaths[]{
      "/",
      "/index.html",
      "/api/v1/users",
      "/api/v1/users/12345/profile",
      "/static/css/main.css",
      "/static/js/app.bundle.js",
      "/favicon.ico",
      "/search?q=hello+world&page=2",
      "/images/logo.png",
      "/api/v1/orders/98765",
      "/checkout",
      "/login",
      "/logout",
      "/account/settings",
      "/blog/2026/07/some-post-title",
  };
  static constexpr std::string_view kHosts[]{
      "www.example.com",
      "api.example.com",
      "cdn.example.net",
      "shop.example.org",
  };
  static constexpr std::string_view kUserAgents[]{
      "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
      "Chrome/128.0.0.0 Safari/537.36",
      "Mozilla/5.0 (Macintosh; Intel Mac OS X 14_5) AppleWebKit/605.1.15 (KHTML, like Gecko) "
      "Version/17.5 Safari/605.1.15",
      "Mozilla/5.0 (X11; Linux x86_64; rv:128.0) Gecko/20100101 Firefox/128.0",
      "curl/8.7.1",
  };
  constexpr std::size_t kRequestCount = 4096;

  // NOLINTNEXTLINE(bugprone-random-generator-seed)
  std::mt19937 rng(0xC0FFEEU);
  std::uniform_int_distribution<std::size_t> methodIndex(0, std::size(kMethods) - 1);
  std::uniform_int_distribution<std::size_t> pathIndex(0, std::size(kPaths) - 1);
  std::uniform_int_distribution<std::size_t> hostIndex(0, std::size(kHosts) - 1);
  std::uniform_int_distribution<std::size_t> userAgentIndex(0, std::size(kUserAgents) - 1);
  std::uniform_int_distribution<int> cookieCount(0, 6);

  Corpus corpus;
  corpus.requests.reserve(kRequestCount);
  for (std::size_t i = 0; i < kRequestCount; ++i) {
    const std::string_view method = kMethods[methodIndex(rng)];
    std::string request;
    request.reserve(700);
    request.append(method).append(" ").append(kPaths[pathIndex(rng)]).append(" HTTP/1.1\r\n");
    request.append("Host: ").append(kHosts[hostIndex(rng)]).append("\r\n");
    request.append("User-Agent: ").append(kUserAgents[userAgentIndex(rng)]).append("\r\n");
    request.append("Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8\r\n");
    request.append("Accept-Language: en-US,en;q=0.5\r\n");
    request.append("Accept-Encoding: gzip, deflate, br\r\n");
    request.append("Connection: keep-alive\r\n");

    const int cookies = cookieCount(rng);
    if (cookies != 0) {
      request.append("Cookie: ");
      for (int cookieIndex = 0; cookieIndex < cookies; ++cookieIndex) {
        if (cookieIndex != 0) {
          request.append("; ");
        }
        request.append("session_id_").append(std::to_string(cookieIndex)).append("=abcdef0123456789");
      }
      request.append("\r\n");
    }
    if (method == "POST" || method == "PUT") {
      request.append("Content-Type: application/json\r\n");
      request.append("Content-Length: 128\r\n");
    }
    request.append("\r\n");

    AddRequestStats(corpus, request);
    corpus.requests.emplace_back(std::move(request));
  }
  return corpus;
}

Corpus BuildLongHeaderCorpus() {
  constexpr std::size_t kRequestCount = 4096;
  Corpus corpus;
  corpus.requests.reserve(kRequestCount);
  for (std::size_t requestIndex = 0; requestIndex < kRequestCount; ++requestIndex) {
    std::string request = "GET /large-headers HTTP/1.1\r\nHost: example.com\r\n";
    request.reserve(1600);
    for (int headerIndex = 0; headerIndex < 5; ++headerIndex) {
      request.append("X-Custom-Header-")
          .append(std::to_string(headerIndex))
          .append(": ")
          .append(280, static_cast<char>('a' + headerIndex))
          .append("\r\n");
    }
    request.append("\r\n");

    AddRequestStats(corpus, request);
    corpus.requests.emplace_back(std::move(request));
  }
  return corpus;
}

const Corpus& TypicalCorpus() {
  static const Corpus corpus = BuildTypicalCorpus();
  return corpus;
}

const Corpus& LongHeaderCorpus() {
  static const Corpus corpus = BuildLongHeaderCorpus();
  return corpus;
}

template <auto SearchFn>
bool MatchesProduction(const Corpus& corpus) {
  for (const std::string& request : corpus.requests) {
    const char* candidateBegin = request.data();
    const char* productionBegin = candidateBegin;
    const char* end = candidateBegin + request.size();
    while (productionBegin < end) {
      const char* productionResult = SearchCRLFProduction(productionBegin, end);
      const char* candidateResult = SearchFn(candidateBegin, end);
      if (candidateResult != productionResult) {
        return false;
      }
      if (productionResult == end) {
        break;
      }
      productionBegin = productionResult + 2;
      candidateBegin = candidateResult + 2;
    }
  }
  return true;
}

template <auto SearchFn>
std::uint64_t ScanCorpus(const Corpus& corpus) {
  std::uint64_t checksum = 0;
  for (const std::string& request : corpus.requests) {
    const char* begin = request.data();
    const char* end = begin + request.size();
    while (begin < end) {
      const char* result = SearchFn(begin, end);
      checksum += static_cast<std::uint64_t>(result - begin);
      if (result == end) {
        break;
      }
      begin = result + 2;
    }
  }
  return checksum;
}

template <auto SearchFn>
void RunBenchmark(benchmark::State& state, const Corpus& corpus) {
  if (!MatchesProduction<SearchFn>(corpus)) {
    state.SkipWithError("candidate results differ from aeronet::SearchCRLF");
    return;
  }

  std::uint64_t checksum = 0;
  for ([[maybe_unused]] auto iteration : state) {
    checksum += ScanCorpus<SearchFn>(corpus);
    benchmark::DoNotOptimize(checksum);
  }

  const auto iterations = static_cast<std::int64_t>(state.iterations());
  const auto requestCount = static_cast<std::int64_t>(corpus.requests.size());
  state.SetItemsProcessed(iterations * requestCount);
  state.SetBytesProcessed(iterations * corpus.totalBytes);
  state.counters["bytes/request"] =
      benchmark::Counter(static_cast<double>(corpus.totalBytes) / static_cast<double>(requestCount));
  state.counters["lines/request"] =
      benchmark::Counter(static_cast<double>(corpus.totalLines) / static_cast<double>(requestCount));
}

template <auto SearchFn>
void RunFixedLineLengthBenchmark(benchmark::State& state) {
  const auto lineLength = static_cast<std::size_t>(state.range(0));
  std::string line(lineLength, 'x');
  line.replace(line.end() - 2U, line.end(), "\r\n");
  const char* const begin = line.data();
  const char* const end = begin + line.size();
  if (SearchFn(begin, end) != begin + (lineLength - 2U)) {
    state.SkipWithError("candidate did not find CRLF at the expected position");
    return;
  }

  for ([[maybe_unused]] auto iteration : state) {
    benchmark::ClobberMemory();
    const char* result = SearchFn(begin, end);
    benchmark::DoNotOptimize(result);
  }

  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()));
  state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations()) * static_cast<std::int64_t>(line.size()));
}

#define AERONET_FIXED_LINE_LENGTHS \
  Arg(16)->Arg(32)->Arg(48)->Arg(64)->Arg(96)->Arg(128)->Arg(192)->Arg(256)->Arg(512)->Arg(1024)

void BM_SearchCRLF_MemchrRestart_FixedLineLength(benchmark::State& state) {
  RunFixedLineLengthBenchmark<SearchCRLFMemchrRestart>(state);
}
BENCHMARK(BM_SearchCRLF_MemchrRestart_FixedLineLength)->AERONET_FIXED_LINE_LENGTHS;

void BM_SearchCRLF_Production_FixedLineLength(benchmark::State& state) {
  RunFixedLineLengthBenchmark<SearchCRLFProduction>(state);
}
BENCHMARK(BM_SearchCRLF_Production_FixedLineLength)->AERONET_FIXED_LINE_LENGTHS;

#ifdef AERONET_BENCH_HAS_SSE2
void BM_SearchCRLF_SSE2_FixedLineLength(benchmark::State& state) { RunFixedLineLengthBenchmark<SearchCRLFSse2>(state); }
BENCHMARK(BM_SearchCRLF_SSE2_FixedLineLength)->AERONET_FIXED_LINE_LENGTHS;
#endif

void BM_SearchCRLF_StringViewFind_FixedLineLength(benchmark::State& state) {
  RunFixedLineLengthBenchmark<SearchCRLFStringViewFind>(state);
}
BENCHMARK(BM_SearchCRLF_StringViewFind_FixedLineLength)->AERONET_FIXED_LINE_LENGTHS;

#undef AERONET_FIXED_LINE_LENGTHS

void BM_SearchCRLF_MemchrRestart_TypicalRequests(benchmark::State& state) {
  RunBenchmark<SearchCRLFMemchrRestart>(state, TypicalCorpus());
}
BENCHMARK(BM_SearchCRLF_MemchrRestart_TypicalRequests);

void BM_SearchCRLF_MemchrRestart_LongHeaders(benchmark::State& state) {
  RunBenchmark<SearchCRLFMemchrRestart>(state, LongHeaderCorpus());
}
BENCHMARK(BM_SearchCRLF_MemchrRestart_LongHeaders);

void BM_SearchCRLF_Production_TypicalRequests(benchmark::State& state) {
  RunBenchmark<SearchCRLFProduction>(state, TypicalCorpus());
}
BENCHMARK(BM_SearchCRLF_Production_TypicalRequests);

void BM_SearchCRLF_Production_LongHeaders(benchmark::State& state) {
  RunBenchmark<SearchCRLFProduction>(state, LongHeaderCorpus());
}
BENCHMARK(BM_SearchCRLF_Production_LongHeaders);

void BM_SearchCRLF_StringViewFind_TypicalRequests(benchmark::State& state) {
  RunBenchmark<SearchCRLFStringViewFind>(state, TypicalCorpus());
}
BENCHMARK(BM_SearchCRLF_StringViewFind_TypicalRequests);

void BM_SearchCRLF_StringViewFind_LongHeaders(benchmark::State& state) {
  RunBenchmark<SearchCRLFStringViewFind>(state, LongHeaderCorpus());
}
BENCHMARK(BM_SearchCRLF_StringViewFind_LongHeaders);

#ifdef AERONET_BENCH_HAS_SSE2
template <std::size_t SsePrefixBytes>
void BM_SearchCRLF_SSE2ThenMemchr_TypicalRequests(benchmark::State& state) {
  RunBenchmark<SearchCRLFSse2ThenMemchr<SsePrefixBytes>>(state, TypicalCorpus());
}

template <std::size_t SsePrefixBytes>
void BM_SearchCRLF_SSE2ThenMemchr_LongHeaders(benchmark::State& state) {
  RunBenchmark<SearchCRLFSse2ThenMemchr<SsePrefixBytes>>(state, LongHeaderCorpus());
}

BENCHMARK_TEMPLATE(BM_SearchCRLF_SSE2ThenMemchr_TypicalRequests, 16);
BENCHMARK_TEMPLATE(BM_SearchCRLF_SSE2ThenMemchr_LongHeaders, 16);
BENCHMARK_TEMPLATE(BM_SearchCRLF_SSE2ThenMemchr_TypicalRequests, 32);
BENCHMARK_TEMPLATE(BM_SearchCRLF_SSE2ThenMemchr_LongHeaders, 32);
BENCHMARK_TEMPLATE(BM_SearchCRLF_SSE2ThenMemchr_TypicalRequests, 64);
BENCHMARK_TEMPLATE(BM_SearchCRLF_SSE2ThenMemchr_LongHeaders, 64);
BENCHMARK_TEMPLATE(BM_SearchCRLF_SSE2ThenMemchr_TypicalRequests, 96);
BENCHMARK_TEMPLATE(BM_SearchCRLF_SSE2ThenMemchr_LongHeaders, 96);
BENCHMARK_TEMPLATE(BM_SearchCRLF_SSE2ThenMemchr_TypicalRequests, 128);
BENCHMARK_TEMPLATE(BM_SearchCRLF_SSE2ThenMemchr_LongHeaders, 128);

void BM_SearchCRLF_SSE2_TypicalRequests(benchmark::State& state) {
  RunBenchmark<SearchCRLFSse2>(state, TypicalCorpus());
}
BENCHMARK(BM_SearchCRLF_SSE2_TypicalRequests);

void BM_SearchCRLF_SSE2_LongHeaders(benchmark::State& state) {
  RunBenchmark<SearchCRLFSse2>(state, LongHeaderCorpus());
}
BENCHMARK(BM_SearchCRLF_SSE2_LongHeaders);
#endif

#ifdef AERONET_BENCH_HAS_AVX2_TARGET
void BM_SearchCRLF_AVX2_TypicalRequests(benchmark::State& state) {
  if (!HasAvx2()) {
    state.SkipWithError("AVX2 is not supported by this CPU");
    return;
  }
  RunBenchmark<SearchCRLFAvx2>(state, TypicalCorpus());
}
BENCHMARK(BM_SearchCRLF_AVX2_TypicalRequests);

void BM_SearchCRLF_AVX2_LongHeaders(benchmark::State& state) {
  if (!HasAvx2()) {
    state.SkipWithError("AVX2 is not supported by this CPU");
    return;
  }
  RunBenchmark<SearchCRLFAvx2>(state, LongHeaderCorpus());
}
BENCHMARK(BM_SearchCRLF_AVX2_LongHeaders);
#endif

}  // namespace

BENCHMARK_MAIN();
