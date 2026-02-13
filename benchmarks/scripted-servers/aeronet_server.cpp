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
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "aeronet/aeronet.hpp"
#include "aeronet/encoding.hpp"
#include "aeronet/ndigits.hpp"
#include "aeronet/static-file-handler.hpp"
#include "aeronet/toupperlower.hpp"
#include "scripted-servers-helpers.hpp"

using namespace aeronet;

namespace {

constexpr std::size_t kCompressionMinBytes = 16UL;  // Compress responses larger than 16 bytes

}

int main(int argc, char* argv[]) {
  bench::BenchConfig benchCfg(8080, argc, argv);

  HttpServerConfig config;
  config.port = benchCfg.port;
  config.nbThreads = static_cast<decltype(config.nbThreads)>(benchCfg.numThreads);
  config.maxRequestsPerConnection = std::numeric_limits<uint32_t>::max();
  config.maxHeaderBytes = 256UL * 1024;  // 256KB headers for stress tests
  config.maxBodyBytes = 64UL << 20;      // 64MB bodies for large body tests
  config.globalHeaders.clear();          // No global headers
  config.compression.addVaryAcceptEncodingHeader = true;
  config.compression.minBytes = kCompressionMinBytes;  // Compress responses larger than 16 bytes
  config.compression.preferredFormats = {Encoding::gzip};

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
    const auto optCount = req.queryParamInt<std::size_t>("count");
    if (!optCount) {
      return req.makeResponse(http::StatusCodeBadRequest);
    }
    const std::size_t count = *optCount;
    const auto optHeaderSize = req.queryParamInt<std::size_t>("size");
    if (!optHeaderSize) {
      return req.makeResponse(http::StatusCodeBadRequest);
    }
    const std::size_t headerSize = *optHeaderSize;

    static constexpr std::string_view kHeaderNamePrefix = "X-Bench-Header-";
    auto resp = req.makeResponse(
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
    auto resp = req.makeResponse(HttpResponse::BodySize(body.size()), http::StatusCodeOK);
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
    const auto optComplexity = req.queryParamInt<int>("complexity");
    if (!optComplexity) {
      return req.makeResponse(http::StatusCodeBadRequest);
    }
    const int complexity = *optComplexity;

    const auto optHashIters = req.queryParamInt<int>("hash_iters");
    if (!optHashIters) {
      return req.makeResponse(http::StatusCodeBadRequest);
    }
    const int hashIters = *optHashIters;

    // Fibonacci computation
    const uint64_t fibResult = bench::Fibonacci(complexity);

    // Hash computation
    const std::string data = std::format("benchmark-data-{}", complexity);
    const uint64_t hashResult = bench::ComputeHash(data, hashIters);
    auto body = std::format("fib({})={}, hash={}", complexity, fibResult, hashResult);

    auto resp = req.makeResponse(64UL + HttpResponse::BodySize(body.size()), http::StatusCodeOK);
    resp.headerAddLine("X-Fib-Result", fibResult);
    resp.headerAddLine("X-Hash-Result", hashResult);
    resp.body(std::move(body));
    return resp;
  });

  // ============================================================
  // Endpoint 5: /json - JSON response test
  // ============================================================
  router.setPath(http::Method::GET, "/json", [](const HttpRequest& req) {
    const std::size_t items = req.queryParamInt<std::size_t>("items").value();

    auto resp = req.makeResponse(items * 40UL, 200);
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
    const int delayMs = req.queryParamInt<int>("ms").value();

    std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));

    return req.makeResponse(std::format("Delayed {} ms", delayMs));
  });

  // ============================================================
  // Endpoint 7: /body - Variable size body test
  // Returns body of size ?size=N bytes
  // ============================================================
  router.setPath(http::Method::GET, "/body", [](const HttpRequest& req) {
    return req.makeResponse(bench::GenerateRandomString(req.queryParamInt<std::size_t>("size").value()));
  });

  // ============================================================
  // Endpoint 7b: /body-codec - Gzip decode/encode stress test
  // aeronet automatically decompresses request body, then we add +1 to each byte
  // returns response that will be automatically compressed
  // ============================================================
  router.setPath(http::Method::POST, "/body-codec", [](const HttpRequest& req) mutable {
    std::string_view body = req.body();
    auto resp = req.makeResponse(http::StatusCodeOK);

    auto buffer = std::make_unique<std::byte[]>(body.size());
    std::transform(body.begin(), body.end(), buffer.get(),
                   [](unsigned char ch) { return static_cast<std::byte>(ch + 1U); });

    resp.body(std::move(buffer), body.size(), "application/octet-stream");
    return resp;
  });

  // ============================================================
  // Endpoint 8: /status - Health check
  // ============================================================
  router.setPath(http::Method::GET, "/status",
                 [numThreads = benchCfg.numThreads, tlsEnabled = benchCfg.tlsEnabled](const HttpRequest& req) {
                   return req.makeResponse(std::format(R"({{"server":"aeronet","threads":{},"tls":{},"status":"ok"}})",
                                                       numThreads, tlsEnabled),
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
