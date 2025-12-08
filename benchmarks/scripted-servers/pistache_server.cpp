// pistache_server.cpp - Pistache benchmark server for wrk testing
//
// Implements standard benchmark endpoints for comparison with other frameworks.
// Build with CMake (requires libpistache-dev or pistache from source)

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include "pistache/endpoint.h"
#include "pistache/http.h"
#include "pistache/http_defs.h"
#include "pistache/mime.h"
#include "pistache/net.h"
#include "scripted-servers-helpers.hpp"

namespace {
constexpr unsigned char toupper(unsigned char ch) {
  if (ch >= 'a' && ch <= 'z') {
    ch &= 0xDF;  // clear lowercase bit
  }
  return ch;
}

constexpr char toupper(char ch) { return static_cast<char>(toupper(static_cast<unsigned char>(ch))); }

// Global config (set before server starts, read-only during request handling)
int gNumThreads = 1;
std::string gStaticDir;
int gRouteCount = 0;

// Parse query parameter as integer
int GetQueryParamOr(const Pistache::Http::Request& req, const std::string& key, int defaultValue) {
  if (req.query().has(key)) {
    try {
      return std::stoi(req.query().get(key).value());
    } catch (...) {
      return defaultValue;
    }
  }
  return defaultValue;
}

uint16_t GetPort() {
  const char* envPort = std::getenv("BENCH_PORT");
  if (envPort != nullptr) {
    return static_cast<uint16_t>(std::atoi(envPort));
  }
  return 8085;
}

std::string GetContentType(std::string_view path) {
  if (path.ends_with(".html")) {
    return "text/html";
  }
  if (path.ends_with(".css")) {
    return "text/css";
  }
  if (path.ends_with(".js")) {
    return "application/javascript";
  }
  if (path.ends_with(".json")) {
    return "application/json";
  }
  return "application/octet-stream";
}

// Parse route number from path like "/r123"
std::optional<int> ParseRouteNumber(const std::string& path) {
  if (path.size() < 2 || path[0] != '/' || path[1] != 'r') {
    return std::nullopt;
  }
  int num = std::stoi(path.substr(2));
  if (num >= 0 && num < gRouteCount) {
    return num;
  }
  return std::nullopt;
}

// Parse pattern like /users/{id}/posts/{post}
struct PatternParams {
  std::string userId;
  std::string postId;
};

std::optional<PatternParams> ParseUserPostPattern(const std::string& path) {
  // /users/{id}/posts/{post}
  if (!path.starts_with("/users/")) {
    return std::nullopt;
  }
  auto postsIdx = path.find("/posts/", 7);
  if (postsIdx == std::string::npos) {
    return std::nullopt;
  }
  return PatternParams{path.substr(7, postsIdx - 7), path.substr(postsIdx + 7)};
}

struct ApiPatternParams {
  std::string resource;
  std::string item;
  std::string action;
};

std::optional<ApiPatternParams> ParseApiPattern(const std::string& path) {
  // /api/v1/resources/{resource}/items/{item}/actions/{action}
  if (!path.starts_with("/api/v1/resources/")) {
    return std::nullopt;
  }
  std::string rest = path.substr(18);  // after /api/v1/resources/
  auto itemsIdx = rest.find("/items/");
  if (itemsIdx == std::string::npos) {
    return std::nullopt;
  }
  auto actionsIdx = rest.find("/actions/", itemsIdx);
  if (actionsIdx == std::string::npos) {
    return std::nullopt;
  }
  return ApiPatternParams{rest.substr(0, itemsIdx), rest.substr(itemsIdx + 7, actionsIdx - itemsIdx - 7),
                          rest.substr(actionsIdx + 9)};
}

// Handler that dispatches to endpoints - all inline to avoid ABI issues
class BenchHandler : public Pistache::Http::Handler {
 public:
  HTTP_PROTOTYPE(BenchHandler)

  void onRequest(const Pistache::Http::Request& req, Pistache::Http::ResponseWriter response) override {
    const std::string& resource = req.resource();
    const auto method = req.method();

    // Strip query string for matching
    auto qpos = resource.find('?');
    std::string path = (qpos != std::string::npos) ? resource.substr(0, qpos) : resource;

    if (method == Pistache::Http::Method::Get && path == "/ping") {
      response.send(Pistache::Http::Code::Ok, "pong");
      return;
    }

    if (method == Pistache::Http::Method::Get && path == "/headers") {
      std::size_t count = static_cast<std::size_t>(GetQueryParamOr(req, "count", 10));
      std::size_t headerSize = static_cast<std::size_t>(GetQueryParamOr(req, "size", 64));
      for (std::size_t pos = 0; pos < count; ++pos) {
        response.headers().addRaw(Pistache::Http::Header::Raw(std::format("X-Bench-Header-{}", pos),
                                                              bench::GenerateRandomString(headerSize)));
      }
      response.send(Pistache::Http::Code::Ok, std::format("Generated {} headers", count));
      return;
    }

    if (method == Pistache::Http::Method::Post && path == "/uppercase") {
      const std::string& body = req.body();
      std::string out;
      out.resize_and_overwrite(body.size(), [&body](char* out, std::size_t n) {
        std::ranges::transform(body, out, [](char ch) { return toupper(ch); });
        return n;
      });
      response.send(Pistache::Http::Code::Ok, out);
      return;
    }

    if (method == Pistache::Http::Method::Get && path == "/compute") {
      int complexity = GetQueryParamOr(req, "complexity", 30);
      int hashIters = GetQueryParamOr(req, "hash_iters", 1000);
      uint64_t fibResult = bench::Fibonacci(complexity);
      std::string data = "benchmark-data-" + std::to_string(complexity);
      uint64_t hashResult = bench::ComputeHash(data, hashIters);
      response.headers().addRaw(Pistache::Http::Header::Raw("X-Fib-Result", std::to_string(fibResult)));
      response.headers().addRaw(Pistache::Http::Header::Raw("X-Hash-Result", std::to_string(hashResult)));
      response.send(Pistache::Http::Code::Ok, std::format("fib({})={}, hash={}", complexity, fibResult, hashResult));
      return;
    }

    if (method == Pistache::Http::Method::Get && path == "/json") {
      std::size_t items = static_cast<std::size_t>(GetQueryParamOr(req, "items", 10));
      std::string json = "{\"items\":[";
      for (std::size_t itemPos = 0; itemPos < items; ++itemPos) {
        if (itemPos > 0) {
          json += ",";
        }
        json += std::format(R"({{"id":{},"name":"item-{}","value":{}}})", itemPos, itemPos, itemPos * 100);
      }
      json += "]}";
      response.headers().addRaw(Pistache::Http::Header::Raw("Content-Type", "application/json"));
      response.send(Pistache::Http::Code::Ok, json);
      return;
    }

    if (method == Pistache::Http::Method::Get && path == "/delay") {
      int delayMs = GetQueryParamOr(req, "ms", 10);
      std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
      response.send(Pistache::Http::Code::Ok, std::format("Delayed {} ms", delayMs));
      return;
    }

    if (method == Pistache::Http::Method::Get && path == "/body") {
      std::size_t size = static_cast<std::size_t>(GetQueryParamOr(req, "size", 1024));
      response.send(Pistache::Http::Code::Ok, bench::GenerateRandomString(size));
      return;
    }

    if (method == Pistache::Http::Method::Get && path == "/status") {
      response.headers().addRaw(Pistache::Http::Header::Raw("Content-Type", "application/json"));
      response.send(Pistache::Http::Code::Ok,
                    std::format(R"({{"server":"pistache","threads":{},"status":"ok"}})", gNumThreads));
      return;
    }

    // Static file serving
    if (method == Pistache::Http::Method::Get && !gStaticDir.empty() && path.starts_with("/")) {
      std::string filePath = path.substr(1);  // strip /
      std::filesystem::path fullPath = std::filesystem::path(gStaticDir) / filePath;

      if (std::filesystem::exists(fullPath) && std::filesystem::is_regular_file(fullPath)) {
        response.headers().addRaw(Pistache::Http::Header::Raw("Content-Type", GetContentType(filePath)));
        Pistache::Http::serveFile(response, fullPath.string(),
                                  Pistache::Http::Mime::MediaType::fromFile(fullPath.filename().c_str()));
      } else {
        response.send(Pistache::Http::Code::Not_Found, "File not found");
      }
      return;
    }

    // Routing stress test: /r{N}
    if (method == Pistache::Http::Method::Get && gRouteCount > 0) {
      if (auto routeNum = ParseRouteNumber(path)) {
        response.send(Pistache::Http::Code::Ok, std::format("route {}", *routeNum));
        return;
      }

      // Pattern route: /users/{id}/posts/{post}
      if (auto params = ParseUserPostPattern(path)) {
        response.send(Pistache::Http::Code::Ok, std::format("user {} post {}", params->userId, params->postId));
        return;
      }

      // Pattern route: /api/v1/resources/{resource}/items/{item}/actions/{action}
      if (auto params = ParseApiPattern(path)) {
        response.send(Pistache::Http::Code::Ok,
                      std::format("resource {} item {} action {}", params->resource, params->item, params->action));
        return;
      }
    }

    response.send(Pistache::Http::Code::Not_Found, "not found");
  }
};

}  // namespace

int main(int argc, char* argv[]) {
  uint16_t port = GetPort();
  int numThreads = bench::GetNumThreads();
  std::string staticDir;
  int routeCount = 0;

  for (int argPos = 1; argPos < argc; ++argPos) {
    std::string arg(argv[argPos]);
    if (arg == "--port" && argPos + 1 < argc) {
      port = static_cast<uint16_t>(std::atoi(argv[++argPos]));
    } else if (arg == "--threads" && argPos + 1 < argc) {
      numThreads = std::atoi(argv[++argPos]);
    } else if (arg == "--static" && argPos + 1 < argc) {
      staticDir = argv[++argPos];
    } else if (arg == "--routes" && argPos + 1 < argc) {
      routeCount = std::atoi(argv[++argPos]);
    } else if (arg == "--help" || arg == "-h") {
      std::cout << "Usage: " << argv[0] << " [options]\n"
                << "Options:\n"
                << "  --port N      Listen port (default: 8085, env: BENCH_PORT)\n"
                << "  --threads N   Worker threads (default: nproc/2, env: BENCH_THREADS)\n"
                << "  --static DIR  Static files directory\n"
                << "  --routes N    Number of /r{N} routes for routing stress test\n"
                << "  --help        Show this help\n";
      return 0;
    }
  }

  // Store in global for handler access
  gNumThreads = numThreads;
  gStaticDir = staticDir;
  gRouteCount = routeCount;

  Pistache::Address addr(Pistache::Ipv4::loopback(), Pistache::Port(port));

  auto opts = Pistache::Http::Endpoint::options()
                  .threads(numThreads)
                  .maxRequestSize(static_cast<size_t>(4ULL * 1024 * 1024 * 1024))
                  .maxResponseSize(static_cast<size_t>(4ULL * 1024 * 1024 * 1024));

  Pistache::Http::Endpoint server(addr);
  server.init(opts);
  server.setHandler(Pistache::Http::make_handler<BenchHandler>());

  std::cout << "pistache benchmark server starting on port " << port << " with " << numThreads << " threads\n";
  if (!staticDir.empty()) {
    std::cout << "Static files: " << staticDir << "\n";
  }
  if (routeCount > 0) {
    std::cout << "Routes: " << routeCount << " literal + pattern routes\n";
  }

  server.serve();

  return 0;
}
