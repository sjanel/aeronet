// aeronet_server.cpp - Benchmark server for wrk testing
//
// Implements standard benchmark endpoints for comparison with other frameworks.
// All endpoints are designed to stress specific aspects of HTTP handling.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "aeronet/aeronet.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/ndigits.hpp"
#include "aeronet/static-file-handler.hpp"
#include "aeronet/stringconv.hpp"
#include "aeronet/toupperlower.hpp"
#include "scripted-servers-helpers.hpp"

using namespace aeronet;

namespace {

// Parse query parameter as integer
template <typename T>
T GetQueryParamOrThrow(const HttpRequest& req, std::string_view key) {
  for (auto qp : req.queryParams()) {
    if (qp.key == key) {
      return StringToIntegral<T>(qp.value);
    }
  }
  throw std::invalid_argument("Query parameter not found");
}

}  // namespace

int main(int argc, char* argv[]) {
  bench::BenchConfig benchCfg(8080, argc, argv);

  log::set_level(log::level::warn);

  HttpServerConfig config;
  config.port = benchCfg.port;
  config.nbThreads = benchCfg.numThreads;
  config.maxRequestsPerConnection = std::numeric_limits<uint32_t>::max();
  config.maxHeaderBytes = 256UL * 1024;  // 256KB headers for stress tests
  config.maxBodyBytes = 64UL << 20;      // 64MB bodies for large body tests
  config.globalHeaders.clear();

  // Configure TLS if enabled
  if (benchCfg.tlsEnabled) {
    if (benchCfg.certFile.empty() || benchCfg.keyFile.empty()) {
      std::cerr << "Error: --tls requires both --cert and --key\n";
      return 1;
    }
    config.withTlsCertKey(benchCfg.certFile, benchCfg.keyFile);
    std::cout << "TLS enabled with cert=" << benchCfg.certFile << " key=" << benchCfg.keyFile << "\n";
  }

  RouterConfig routerConfig;
  routerConfig.trailingSlashPolicy = RouterConfig::TrailingSlashPolicy::Strict;
  Router router(std::move(routerConfig));

  // ============================================================
  // Endpoint 1: /ping - Minimal latency test
  // ============================================================
  router.setPath(http::Method::GET, "/ping", [](const HttpRequest& req) { return req.makeResponse("pong"); });

  // ============================================================
  // Endpoint 2: /headers - Header stress test
  // Returns N headers based on ?count=N query param
  // ============================================================
  router.setPath(http::Method::GET, "/headers", [](const HttpRequest& req) {
    std::size_t count = 10;
    std::size_t headerSize = 64;
    for (auto qp : req.queryParams()) {
      if (qp.key == "count") {
        count = StringToIntegral<std::size_t>(qp.value);
      } else if (qp.key == "size") {
        headerSize = StringToIntegral<std::size_t>(qp.value);
      }
    }
    static constexpr std::string_view kHeaderNamePrefix = "X-Bench-Header-";
    HttpResponse resp = req.makeResponse(
        count * HttpResponse::HeaderSize(kHeaderNamePrefix.size() + ndigits(count), headerSize), http::StatusCodeOK);
    for (std::size_t headerPos = 0; headerPos < count; ++headerPos) {
      resp.headerAddLine(std::format("{}{}", kHeaderNamePrefix, headerPos), bench::GenerateRandomString(headerSize));
    }
    resp.body(std::format("Generated {} headers", count));
    return resp;
  });

  // ============================================================
  // Endpoint 3: /uppercase - Body uppercase test
  // Echoes POST body back in response (force allocate uppercase copy)
  // ============================================================
  router.setPath(http::Method::POST, "/uppercase", [](const HttpRequest& req) {
    std::string_view body = req.body();
    HttpResponse resp = req.makeResponse(HttpResponse::BodySize(body.size()), http::StatusCodeOK);
    resp.bodyInlineSet(body.size(), [body](char* buf) {
      std::ranges::transform(body, buf, [](char ch) { return toupper(ch); });
      return body.size();
    });
    return resp;
  });

  // ============================================================
  // Endpoint 4: /compute - CPU-bound test
  // Performs expensive computation based on ?complexity=N
  // ============================================================
  router.setPath(http::Method::GET, "/compute", [](const HttpRequest& req) {
    int complexity = 10;
    int hashIters = 64;
    for (auto qp : req.queryParams()) {
      if (qp.key == "complexity") {
        complexity = StringToIntegral<int>(qp.value);
      } else if (qp.key == "hash_iters") {
        hashIters = StringToIntegral<int>(qp.value);
      }
    }

    // Fibonacci computation
    const uint64_t fibResult = bench::Fibonacci(complexity);

    // Hash computation
    const std::string data = std::format("benchmark-data-{}", complexity);
    const uint64_t hashResult = bench::ComputeHash(data, hashIters);
    auto body = std::format("fib({})={}, hash={}", complexity, fibResult, hashResult);

    HttpResponse resp = req.makeResponse(64UL + HttpResponse::BodySize(body.size()), http::StatusCodeOK);
    resp.headerAddLine("X-Fib-Result", fibResult);
    resp.headerAddLine("X-Hash-Result", hashResult);
    resp.body(std::move(body));
    return resp;
  });

  // ============================================================
  // Endpoint 5: /json - JSON response test
  // ============================================================
  router.setPath(http::Method::GET, "/json", [](const HttpRequest& req) {
    const std::size_t items = GetQueryParamOrThrow<std::size_t>(req, "items");

    HttpResponse resp = req.makeResponse(200);
    resp.bodyAppend("{\"items\":[", "application/json");
    for (std::size_t itemPos = 0; itemPos < items; ++itemPos) {
      if (itemPos > 0) {
        resp.bodyAppend(",");
      }
      resp.bodyAppend(std::format(R"({{"id":{},"name":"item-{}","value":{}}})", itemPos, itemPos, itemPos * 100));
    }
    resp.bodyAppend("]}");

    return resp;
  });

  // ============================================================
  // Endpoint 6: /delay - Artificial delay test
  // Sleeps for ?ms=N milliseconds
  // ============================================================
  router.setPath(http::Method::GET, "/delay", [](const HttpRequest& req) {
    const int delayMs = GetQueryParamOrThrow<int>(req, "ms");

    std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));

    return req.makeResponse(std::format("Delayed {} ms", delayMs));
  });

  // ============================================================
  // Endpoint 7: /body - Variable size body test
  // Returns body of size ?size=N bytes
  // ============================================================
  router.setPath(http::Method::GET, "/body", [](const HttpRequest& req) {
    return req.makeResponse(bench::GenerateRandomString(GetQueryParamOrThrow<std::size_t>(req, "size")));
  });

  // ============================================================
  // Endpoint 8: /status - Health check
  // ============================================================
  router.setPath(http::Method::GET, "/status", [&benchCfg](const HttpRequest& req) {
    return req.makeResponse(std::format(R"({{"server":"aeronet","threads":{},"tls":{},"status":"ok"}})",
                                        benchCfg.numThreads, benchCfg.tlsEnabled),
                            "application/json");
  });

  // ============================================================
  // Endpoint 9: /* - Static file serving (if --static DIR given)
  // ============================================================
  if (!benchCfg.staticDir.empty()) {
    std::filesystem::path staticPath(benchCfg.staticDir);
    if (std::filesystem::is_directory(staticPath)) {
      StaticFileConfig staticCfg;
      // For benchmark runs we want to avoid extra per-request filesystem/stat work
      // (ETag/If-Modified/Last-Modified) which other frameworks sometimes skip
      // when they serve cached content at the handler level. Disable conditional
      // checks and related headers for a fairer comparison of raw send-path.
      staticCfg.enableRange = true;
      staticCfg.enableConditional = false;
      staticCfg.addLastModified = false;
      staticCfg.addEtag = false;
      staticCfg.enableDirectoryIndex = false;

      router.setDefault(StaticFileHandler(std::move(staticPath), staticCfg));

      std::cout << "Static file serving enabled at /* from " << benchCfg.staticDir << "\n";
    } else {
      std::cerr << "Warning: static directory does not exist: " << benchCfg.staticDir << "\n";
    }
  }

  // ============================================================
  // Endpoint 10+: /r{N} - Routing stress test (N literal routes)
  // ============================================================
  for (int routeIdx = 0; routeIdx < benchCfg.routeCount; ++routeIdx) {
    router.setPath(http::Method::GET, std::format("/r{}", routeIdx),
                   [routeIdx](const HttpRequest& req) { return req.makeResponse(std::format("route-{}", routeIdx)); });
  }
  std::cout << "Registered " << benchCfg.routeCount << " literal routes (/r0 to /r" << (benchCfg.routeCount - 1)
            << ")\n";

  // ============================================================
  // Endpoint: /users/{id}/posts/{post} - Pattern matching stress test
  // ============================================================
  router.setPath(http::Method::GET, "/users/{id}/posts/{post}", [](const HttpRequest& req) {
    const auto& params = req.pathParams();
    std::string_view userId = params.find("id")->second;
    std::string_view postId = params.find("post")->second;
    return req.makeResponse(std::format("user={},post={}", userId, postId));
  });

  // ============================================================
  // Endpoint: /api/v1/resources/{resource}/items/{item}/actions/{action} - Another pattern route
  // ============================================================
  router.setPath(http::Method::GET, "/api/v1/resources/{resource}/items/{item}/actions/{action}",
                 [](const HttpRequest& req) {
                   const auto& params = req.pathParams();
                   std::string_view resource = params.find("resource")->second;
                   std::string_view item = params.find("item")->second;
                   std::string_view action = params.find("action")->second;
                   return req.makeResponse(std::format("resource={},item={},action={}", resource, item, action));
                 });

  std::cout << "aeronet benchmark server starting on port " << benchCfg.port << " with " << benchCfg.numThreads
            << " threads\n";

  SignalHandler::Enable();

  HttpServer server(std::move(config), std::move(router));

  server.run();  // Blocking call - will return on SIGINT/SIGTERM

  return 0;
}
