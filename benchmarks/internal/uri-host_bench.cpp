// Cost of the validation of the Host header field (RFC 9110 §7.2), run for each request by the HTTP/1 parser, compared
// with the scan of the header value bytes that the parser runs for every header field.
// Run:
//   ./aeronet-bench-internal-uri-host --benchmark_counters_tabular=true
#include <benchmark/benchmark.h>

#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>

#include "aeronet/http-header-is-valid.hpp"
#include "aeronet/uri-host.hpp"

namespace aeronet::http {

namespace {

constexpr std::string_view kHosts[]{
    "localhost",
    "example.com",
    "www.example.com",
    "localhost:8080",
    "api.internal.example.com",
    "api.internal.example.com:8443",
    "192.168.100.200:8080",
    "[::1]:8080",
    "[2001:db8:85a3::8a2e:370:7334]:443",
};

template <bool (*kValidate)(std::string_view)>
void ValidateHost(benchmark::State& state) {
  const std::string_view host = kHosts[state.range(0)];
  int64_t nbValid = 0;
  for (auto st : state) {
    std::string_view value = host;
    benchmark::DoNotOptimize(value);
    nbValid += static_cast<int64_t>(kValidate(value));
  }
  state.SetLabel(std::string(host));
  state.SetBytesProcessed(static_cast<int64_t>(state.iterations() * host.size()));
  state.counters["valid"] = benchmark::Counter(static_cast<double>(nbValid == state.iterations()));
}

bool HostFieldValue(std::string_view value) { return IsValidHostFieldValue(value); }
bool HeaderValue(std::string_view value) { return IsValidHeaderValue(value); }

}  // namespace

BENCHMARK(ValidateHost<HostFieldValue>)->DenseRange(0, std::size(kHosts) - 1);
BENCHMARK(ValidateHost<HeaderValue>)->DenseRange(0, std::size(kHosts) - 1);

}  // namespace aeronet::http

BENCHMARK_MAIN();
