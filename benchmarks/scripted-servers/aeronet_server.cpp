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

uint16_t GetPort() {
  const char* envPort = std::getenv("BENCH_PORT");
  if (envPort != nullptr) {
    return static_cast<uint16_t>(std::atoi(envPort));
  }
  return 8080;
}

struct BenchConfig {
  uint16_t port{8080};
  int numThreads{1};
  bool tlsEnabled{false};
  std::string certFile;
  std::string keyFile;
  std::string staticDir;
  int routeCount{1000};  // Number of literal routes for routing stress test
};

BenchConfig ParseArgs(int argc, char* argv[]) {
  BenchConfig cfg;
  cfg.port = GetPort();
  cfg.numThreads = bench::GetNumThreads();

  for (int argPos = 1; argPos < argc; ++argPos) {
    std::string_view arg(argv[argPos]);
    if (arg == "--port" && argPos + 1 < argc) {
      cfg.port = static_cast<uint16_t>(std::atoi(argv[++argPos]));
    } else if (arg == "--threads" && argPos + 1 < argc) {
      cfg.numThreads = std::atoi(argv[++argPos]);
    } else if (arg == "--tls") {
      cfg.tlsEnabled = true;
    } else if (arg == "--cert" && argPos + 1 < argc) {
      cfg.certFile = argv[++argPos];
    } else if (arg == "--key" && argPos + 1 < argc) {
      cfg.keyFile = argv[++argPos];
    } else if (arg == "--static" && argPos + 1 < argc) {
      cfg.staticDir = argv[++argPos];
    } else if (arg == "--routes" && argPos + 1 < argc) {
      cfg.routeCount = std::atoi(argv[++argPos]);
    } else if (arg == "--help" || arg == "-h") {
      std::cout << "Usage: " << argv[0] << " [options]\n"
                << "Options:\n"
                << "  --port N      Listen port (default: 8080, env: BENCH_PORT)\n"
                << "  --threads N   Worker threads (default: nproc/2, env: BENCH_THREADS)\n"
                << "  --tls         Enable TLS (requires --cert and --key)\n"
                << "  --cert FILE   TLS certificate file (PEM)\n"
                << "  --key FILE    TLS private key file (PEM)\n"
                << "  --static DIR  Directory for static file serving\n"
                << "  --routes N    Number of literal routes (default: 1000)\n"
                << "  --help        Show this help\n";
      std::exit(0);
    }
  }
  return cfg;
}

}  // namespace

int main(int argc, char* argv[]) {
  BenchConfig benchCfg = ParseArgs(argc, argv);

  log::set_level(log::level::warn);

  HttpServerConfig config;
  config.port = benchCfg.port;
  config.nbThreads = benchCfg.numThreads;
  config.maxRequestsPerConnection = std::numeric_limits<uint32_t>::max();
  config.maxHeaderBytes = 256UL * 1024;  // 256KB headers for stress tests
  config.maxBodyBytes = 64UL << 20;      // 64MB bodies for large body tests
  config.reusePort = true;
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

  Router router;

  // ============================================================
  // Endpoint 1: /ping - Minimal latency test
  // ============================================================
  router.setPath(http::Method::GET, "/ping", [](const HttpRequest&) { return HttpResponse("pong"); });

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
    HttpResponse resp(200);
    for (std::size_t headerPos = 0; headerPos < count; ++headerPos) {
      resp.addHeader(std::format("X-Bench-Header-{}", headerPos), bench::GenerateRandomString(headerSize));
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
    HttpResponse resp(200);
    resp.appendBody(body.size(), [body](char* buf) {
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

    HttpResponse resp(200);
    resp.addHeader("X-Fib-Result", fibResult);
    resp.addHeader("X-Hash-Result", hashResult);
    resp.body(std::format("fib({})={}, hash={}", complexity, fibResult, hashResult));
    return resp;
  });

  // ============================================================
  // Endpoint 5: /json - JSON response test
  // ============================================================
  router.setPath(http::Method::GET, "/json", [](const HttpRequest& req) {
    const std::size_t items = GetQueryParamOrThrow<std::size_t>(req, "items");

    std::string json = "{\"items\":[";
    for (std::size_t itemPos = 0; itemPos < items; ++itemPos) {
      if (itemPos > 0) {
        json += ",";
      }
      json += std::format(R"({{"id":{},"name":"item-{}","value":{}}})", itemPos, itemPos, itemPos * 100);
    }
    json += "]}";

    return HttpResponse(std::move(json), http::ContentTypeApplicationJson);
  });

  // ============================================================
  // Endpoint 6: /delay - Artificial delay test
  // Sleeps for ?ms=N milliseconds
  // ============================================================
  router.setPath(http::Method::GET, "/delay", [](const HttpRequest& req) {
    const int delayMs = GetQueryParamOrThrow<int>(req, "ms");

    std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));

    return HttpResponse(std::format("Delayed {} ms", delayMs));
  });

  // ============================================================
  // Endpoint 7: /body - Variable size body test
  // Returns body of size ?size=N bytes
  // ============================================================
  router.setPath(http::Method::GET, "/body", [](const HttpRequest& req) {
    HttpResponse resp(200);
    resp.body(bench::GenerateRandomString(GetQueryParamOrThrow<std::size_t>(req, "size")));
    return resp;
  });

  // ============================================================
  // Endpoint 8: /status - Health check
  // ============================================================
  router.setPath(http::Method::GET, "/status", [&benchCfg](const HttpRequest&) {
    return HttpResponse(std::format(R"({{"server":"aeronet","threads":{},"tls":{},"status":"ok"}})",
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
    std::string path = std::format("/r{}", routeIdx);
    router.setPath(http::Method::GET, path,
                   [routeIdx](const HttpRequest&) { return HttpResponse(std::format("route-{}", routeIdx)); });
  }
  std::cout << "Registered " << benchCfg.routeCount << " literal routes (/r0 to /r" << (benchCfg.routeCount - 1)
            << ")\n";

  // ============================================================
  // Endpoint: /users/{id}/posts/{post} - Pattern matching stress test
  // ============================================================
  router.setPath(http::Method::GET, "/users/{id}/posts/{post}", [](const HttpRequest& req) {
    std::string_view userId;
    std::string_view postId;
    for (auto [key, value] : req.pathParams()) {
      if (key == "id") {
        userId = value;
      } else if (key == "post") {
        postId = value;
      }
    }
    return HttpResponse(std::format("user={},post={}", userId, postId));
  });

  // ============================================================
  // Endpoint: /api/v{version}/items/{item} - Another pattern route
  // ============================================================
  router.setPath(http::Method::GET, "/api/v{version}/items/{item}", [](const HttpRequest& req) {
    std::string_view version;
    std::string_view item;
    for (auto [key, value] : req.pathParams()) {
      if (key == "version") {
        version = value;
      } else if (key == "item") {
        item = value;
      }
    }
    return HttpResponse(std::format("v={},item={}", version, item));
  });

  std::cout << "aeronet benchmark server starting on port " << benchCfg.port << " with " << benchCfg.numThreads
            << " threads\n";

  SignalHandler::Enable();

  HttpServer server(std::move(config), std::move(router));

  server.run();  // Blocking call - will return on SIGINT/SIGTERM

  return 0;
}
