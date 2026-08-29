#include <benchmark/benchmark.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "aeronet/http-client.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request-view.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/response-cache.hpp"
#include "aeronet/router.hpp"
#include "aeronet/single-http-server.hpp"

namespace aeronet {
namespace {

std::string ComputePayload() {
  static std::atomic_uint64_t invocation{};
  uint64_t value = 0x9E3779B97F4A7C15ULL ^ invocation.fetch_add(1U, std::memory_order_relaxed);
  for (unsigned iteration = 0; iteration < 32768U; ++iteration) {
    value ^= value << 13U;
    value ^= value >> 7U;
    value ^= value << 17U;
  }
  return std::to_string(value);
}

class ResponseCacheBenchmarkServer {
 public:
  ResponseCacheBenchmarkServer() {
    Router router;
    router
        .setPath(http::Method::GET, "/cached",
                 [](const HttpRequestView& request) {
                   auto response = request.makeResponse(ComputePayload());
                   response.headerAddLine(http::CacheControl, "max-age=3600");
                   return response;
                 })
        .cache(_cache);
    router.setPath(http::Method::GET, "/uncached",
                   [](const HttpRequestView& request) { return request.makeResponse(ComputePayload()); });

    _server = std::make_unique<SingleHttpServer>(
        HttpServerConfig{}.withPort(0).withPollInterval(std::chrono::milliseconds{10}), std::move(router));
    const std::string origin = "http://127.0.0.1:" + std::to_string(_server->port());
    _cachedUrl = origin + "/cached";
    _uncachedUrl = origin + "/uncached";
    _server->start();
    benchmark::DoNotOptimize(_client.get(_cachedUrl));  // populate before measurements
  }

  [[nodiscard]] HttpClientResult cached() { return _client.get(_cachedUrl); }
  [[nodiscard]] HttpClientResult uncached() { return _client.get(_uncachedUrl); }

 private:
  ResponseCache _cache;
  std::unique_ptr<SingleHttpServer> _server;
  HttpClient _client;
  std::string _cachedUrl;
  std::string _uncachedUrl;
};

ResponseCacheBenchmarkServer& Server() {
  static ResponseCacheBenchmarkServer server;
  return server;
}

void BM_ResponseCacheHit(benchmark::State& state) {
  auto& server = Server();
  for ([[maybe_unused]] auto iteration : state) {
    auto result = server.cached();
    if (!result) {
      state.SkipWithError("cached loopback request failed");
      break;
    }
    benchmark::DoNotOptimize(result->bodyInMemory().data());
  }
}

void BM_ResponseCacheUncachedHandler(benchmark::State& state) {
  auto& server = Server();
  for ([[maybe_unused]] auto iteration : state) {
    auto result = server.uncached();
    if (!result) {
      state.SkipWithError("uncached loopback request failed");
      break;
    }
    benchmark::DoNotOptimize(result->bodyInMemory().data());
  }
}

BENCHMARK(BM_ResponseCacheHit);
BENCHMARK(BM_ResponseCacheUncachedHandler);

}  // namespace
}  // namespace aeronet

BENCHMARK_MAIN();
