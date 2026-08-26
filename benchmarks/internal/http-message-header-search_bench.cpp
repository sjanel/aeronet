#include <benchmark/benchmark.h>

#include <cstddef>
#include <string>
#include <string_view>

#include "aeronet/http-response.hpp"
#include "aeronet/lower-ascii-key.hpp"

namespace aeronet {
namespace {

HttpResponse BuildResponse(std::size_t headerCount, bool addTarget) {
  HttpResponse response;
  for (std::size_t index = 0; index < headerCount; ++index) {
    const std::string headerName = "x-benchmark-header-" + std::to_string(index);
    response.headerAddLine(LowerAsciiKey{headerName},
                           "representative-header-value-" + std::to_string(index));
  }
  if (addTarget) {
    response.headerAddLine("content-language", "en-US");
  }
  return response;
}

void BM_HeaderValuePresentAtEnd(benchmark::State& state) {
  const auto response = BuildResponse(static_cast<std::size_t>(state.range(0)), true);

  for (auto _ : state) {
    benchmark::DoNotOptimize(response.headerValue("content-language"));
  }

  state.SetItemsProcessed(state.iterations());
  state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(response.headersFlatView().size()));
}

void BM_HeaderValueAbsent(benchmark::State& state) {
  const auto response = BuildResponse(static_cast<std::size_t>(state.range(0)), false);

  for (auto _ : state) {
    benchmark::DoNotOptimize(response.headerValue("content-language"));
  }

  state.SetItemsProcessed(state.iterations());
  state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(response.headersFlatView().size()));
}

BENCHMARK(BM_HeaderValuePresentAtEnd)->Arg(10)->Arg(35)->Arg(100);
BENCHMARK(BM_HeaderValueAbsent)->Arg(10)->Arg(35)->Arg(100);

}  // namespace
}  // namespace aeronet

BENCHMARK_MAIN();
