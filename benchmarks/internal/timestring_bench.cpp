// Micro-benchmark of the HTTP date parsing of timestring.hpp (If-Modified-Since, If-Unmodified-Since and If-Range of
// the static file handler), and of the 3-letter month lookup that the cookie dates of HttpClient share.
//
// The argument is the number of distinct dates, cycled through: 1024 are learned by the branch predictor of the CPU,
// 65536 are not. The month and weekday lookups are a perfect hash, so that both cost the same: a linear search over the
// abbreviations took twice as long for unpredictable dates.
#include <benchmark/benchmark.h>

#include <chrono>
#include <cstdint>
#include <random>
#include <string>

#include "aeronet/time-constants.hpp"
#include "aeronet/timedef.hpp"
#include "aeronet/timestring.hpp"
#include "aeronet/vector.hpp"

namespace aeronet {
namespace {

// 'nbDates' (a power of 2) random dates of 1990 to 2050, in IMF-fixdate format.
vector<std::string> RandomDates(int64_t nbDates) {
  std::mt19937_64 rng(42);
  std::uniform_int_distribution<int64_t> seconds(20LL * 365 * 86400, 80LL * 365 * 86400);
  vector<std::string> dates;
  dates.reserve(static_cast<vector<std::string>::size_type>(nbDates));
  for (int64_t dateIdx = 0; dateIdx < nbDates; ++dateIdx) {
    char buf[RFC7231DateStrLen];
    TimeToStringRFC7231(SysTimePoint{std::chrono::seconds{seconds(rng)}}, buf);
    dates.emplace_back(buf, sizeof(buf));
  }
  return dates;
}

void BM_TryParseTimeRFC7231(benchmark::State& state) {
  const vector<std::string> dates = RandomDates(state.range(0));
  const auto mask = dates.size() - 1U;
  vector<std::string>::size_type dateIdx = 0;
  for (auto _ : state) {
    benchmark::DoNotOptimize(TryParseTimeRFC7231(dates[dateIdx++ & mask]));
  }
}
BENCHMARK(BM_TryParseTimeRFC7231)->Arg(1024)->Arg(65536);

void BM_MonthOfAbbreviation(benchmark::State& state) {
  const vector<std::string> dates = RandomDates(state.range(0));
  const auto mask = dates.size() - 1U;
  vector<std::string>::size_type dateIdx = 0;
  for (auto _ : state) {
    benchmark::DoNotOptimize(MonthOfAbbreviation(dates[dateIdx++ & mask].data() + 8));
  }
}
BENCHMARK(BM_MonthOfAbbreviation)->Arg(1024)->Arg(65536);

}  // namespace
}  // namespace aeronet

BENCHMARK_MAIN();
