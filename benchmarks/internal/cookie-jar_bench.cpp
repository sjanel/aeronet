// Micro-benchmark for the CookieJar of HttpClient: the Cookie header of a request (once per exchange when the cookies
// are enabled), and the storage of the Set-Cookie headers of a response.
//
// The public cookieHeader() returns a std::string: its allocation is part of the measure, while HttpClient builds the
// header in a reused buffer.
#include <benchmark/benchmark.h>

#include <chrono>
#include <cstdint>
#include <string>

#include "aeronet/cookie-jar.hpp"
#include "aeronet/timedef.hpp"

namespace aeronet {
namespace {

const SysTimePoint kNow = std::chrono::sys_days{std::chrono::year{2026} / 11 / 7};

// A jar of 'nbDomains' sites of 'nbCookiesPerDomain' cookies each, set by https://wwwN.example<N>.com/. A refused
// cookie skips the benchmark, which would measure a smaller jar.
CookieJar MakeJar(benchmark::State& state, int64_t nbDomains, int64_t nbCookiesPerDomain) {
  CookieJar jar;
  for (int64_t domainIdx = 0; domainIdx < nbDomains; ++domainIdx) {
    const std::string url = "https://www.example" + std::to_string(domainIdx) + ".com/";
    for (int64_t cookieIdx = 0; cookieIdx < nbCookiesPerDomain; ++cookieIdx) {
      if (!jar.setCookie(url, "cookie" + std::to_string(cookieIdx) + "=0123456789abcdef; Path=/; Secure; HttpOnly",
                         kNow)) {
        state.SkipWithError("a cookie of the jar was refused");
        return jar;
      }
    }
  }
  return jar;
}

// A request to a site without cookies, in a jar holding those of many sites: one hash lookup.
void BM_CookieHeaderNoMatch(benchmark::State& state) {
  const CookieJar jar = MakeJar(state, state.range(0), 5);
  for (auto _ : state) {
    std::string header = jar.cookieHeader("https://www.other.com/path/page?query=1", kNow);
    benchmark::DoNotOptimize(header);
  }
}
BENCHMARK(BM_CookieHeaderNoMatch)->Arg(1)->Arg(100)->Arg(1000);

// A request to a site with 'range(0)' matching cookies.
void BM_CookieHeader(benchmark::State& state) {
  const CookieJar jar = MakeJar(state, 1, state.range(0));
  for (auto _ : state) {
    std::string header = jar.cookieHeader("https://www.example0.com/path/page?query=1", kNow);
    benchmark::DoNotOptimize(header);
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_CookieHeader)->Arg(1)->Arg(10)->Arg(50)->Arg(180);

// A response refreshing a session cookie of a site of 'range(0)' cookies: parsing (Expires date included) and
// replacement in place.
void BM_SetCookieReplace(benchmark::State& state) {
  CookieJar jar = MakeJar(state, 1, state.range(0));
  for (auto _ : state) {
    bool stored = jar.setCookie(
        "https://www.example0.com/login",
        "cookie0=fedcba9876543210; Path=/; Expires=Wed, 09 Jun 2027 10:18:14 GMT; Secure; HttpOnly; SameSite=Lax",
        kNow);
    benchmark::DoNotOptimize(stored);
  }
}
BENCHMARK(BM_SetCookieReplace)->Arg(1)->Arg(10)->Arg(180);

// A new cookie in a site of 'range(0)' cookies, then its deletion (Max-Age=0): one insertion and one removal.
void BM_SetCookieInsertAndDelete(benchmark::State& state) {
  CookieJar jar = MakeJar(state, 1, state.range(0));
  for (auto _ : state) {
    bool inserted = jar.setCookie("https://www.example0.com/", "fresh=1; Path=/", kNow);
    bool deleted = jar.setCookie("https://www.example0.com/", "fresh=; Path=/; Max-Age=0", kNow);
    benchmark::DoNotOptimize(inserted);
    benchmark::DoNotOptimize(deleted);
  }
}
BENCHMARK(BM_SetCookieInsertAndDelete)->Arg(1)->Arg(10)->Arg(170);

}  // namespace
}  // namespace aeronet

BENCHMARK_MAIN();
