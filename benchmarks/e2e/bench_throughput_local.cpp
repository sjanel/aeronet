#include <benchmark/benchmark.h>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"

// Throughput skeleton converted to Google Benchmark. Currently just polls server.port()
// to keep the loop structure similar; later this can evolve into an actual client loop.

namespace {
void BenchThroughputSkeleton(benchmark::State& state) {
  aeronet::HttpServer server(aeronet::HttpServerConfig{}.withPort(0));
  server.router().setDefault([](const aeronet::HttpRequest&) {
    aeronet::HttpResponse resp;
    resp.body("OK");
    return resp;
  });
  for ([[maybe_unused]] auto it : state) {
    benchmark::DoNotOptimize(server.port());
  }
  state.counters["iterations"] = static_cast<double>(state.iterations());
}
}  // namespace

BENCHMARK(BenchThroughputSkeleton);

BENCHMARK_MAIN();
