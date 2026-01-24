// crow_server.cpp - Crow benchmark server for wrk testing
//
// Implements the same endpoints as aeronet_server.cpp for fair comparison.
// Requires AERONET_BENCH_ENABLE_CROW=ON during CMake configuration.
// Uses CrowCpp/Crow (the maintained fork): https://github.com/CrowCpp/Crow

#include <crow.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

#include "scripted-servers-helpers.hpp"

namespace {

constexpr unsigned char toupper(unsigned char ch) {
  if (ch >= 'a' && ch <= 'z') {
    ch &= 0xDF;  // clear lowercase bit
  }
  return ch;
}

constexpr char toupper(char ch) { return static_cast<char>(toupper(static_cast<unsigned char>(ch))); }

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

// Read file content as string
std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return {};
  }
  return std::string{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

}  // namespace

int main(int argc, char* argv[]) {
  bench::BenchConfig benchCfg(8087, argc, argv);

  crow::SimpleApp app;

  // Disable Crow's logging for benchmark mode
  app.loglevel(crow::LogLevel::Warning);

  // ============================================================
  // Endpoint 1: /ping - Minimal latency test
  // ============================================================
  CROW_ROUTE(app, "/ping")
  ([]() { return "pong"; });

  // ============================================================
  // Endpoint 2: /headers - Header stress test
  // ============================================================
  CROW_ROUTE(app, "/headers")
  ([](const crow::request& req) {
    std::size_t count = 10;
    std::size_t headerSize = 64;

    if (auto countParam = req.url_params.get("count"); countParam != nullptr) {
      count = static_cast<std::size_t>(std::stoull(countParam));
    }
    if (auto sizeParam = req.url_params.get("size"); sizeParam != nullptr) {
      headerSize = static_cast<std::size_t>(std::stoull(sizeParam));
    }

    crow::response res(200);
    for (std::size_t headerPos = 0; headerPos < count; ++headerPos) {
      res.add_header(std::format("X-Bench-Header-{}", headerPos), bench::GenerateRandomString(headerSize));
    }
    res.body = std::format("Generated {} headers", count);
    return res;
  });

  // ============================================================
  // Endpoint 3: /uppercase - Body uppercase test
  // ============================================================
  CROW_ROUTE(app, "/uppercase").methods(crow::HTTPMethod::POST)([](const crow::request& req) {
    std::string_view body = req.body;
    std::string out;
    out.resize_and_overwrite(body.size(), [body](char* out, std::size_t n) {
      std::ranges::transform(body, out, [](char ch) { return toupper(ch); });
      return n;
    });
    return out;
  });

  // ============================================================
  // Endpoint 4: /compute - CPU-bound test
  // ============================================================
  CROW_ROUTE(app, "/compute")
  ([](const crow::request& req) {
    int complexity = 30;
    int hashIters = 1000;

    if (auto complexityParam = req.url_params.get("complexity"); complexityParam != nullptr) {
      complexity = std::stoi(complexityParam);
    }
    if (auto hashParam = req.url_params.get("hash_iters"); hashParam != nullptr) {
      hashIters = std::stoi(hashParam);
    }

    uint64_t fibResult = bench::Fibonacci(complexity);
    std::string data = std::format("benchmark-data-{}", complexity);
    uint64_t hashResult = bench::ComputeHash(data, hashIters);

    crow::response res(200);
    res.add_header("X-Fib-Result", std::to_string(fibResult));
    res.add_header("X-Hash-Result", std::to_string(hashResult));
    res.body = std::format("fib({})={}, hash={}", complexity, fibResult, hashResult);
    return res;
  });

  // ============================================================
  // Endpoint 5: /json - JSON response test
  // ============================================================
  CROW_ROUTE(app, "/json")
  ([](const crow::request& req) {
    std::size_t items = 10;
    if (auto itemsParam = req.url_params.get("items"); itemsParam != nullptr) {
      items = static_cast<std::size_t>(std::stoull(itemsParam));
    }

    crow::response res(200);
    res.add_header("Content-Type", "application/json");
    res.body = bench::BuildJson(items);
    return res;
  });

  // ============================================================
  // Endpoint 6: /delay - Artificial delay test
  // ============================================================
  CROW_ROUTE(app, "/delay")
  ([](const crow::request& req) {
    int delayMs = 10;
    if (auto msParam = req.url_params.get("ms"); msParam != nullptr) {
      delayMs = std::stoi(msParam);
    }

    // Synchronous delay (Crow doesn't have built-in async delay like Drogon)
    std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));

    return std::format("Delayed {} ms", delayMs);
  });

  // ============================================================
  // Endpoint 7: /body - Variable size body test
  // ============================================================
  CROW_ROUTE(app, "/body")
  ([](const crow::request& req) {
    std::size_t size = 1024;
    if (auto sizeParam = req.url_params.get("size"); sizeParam != nullptr) {
      size = static_cast<std::size_t>(std::stoull(sizeParam));
    }

    return bench::GenerateRandomString(size);
  });

  // ============================================================
  // Endpoint 8: /status - Health check
  // ============================================================
  CROW_ROUTE(app, "/status")
  ([numThreads = benchCfg.numThreads]() {
    crow::response res(200);
    res.add_header("Content-Type", "application/json");
    res.body = std::format(R"({{"server":"crow","threads":{},"status":"ok"}})", numThreads);
    return res;
  });

  // ============================================================
  // Endpoint 9: Static file serving (catch-all for files)
  // ============================================================
  if (!benchCfg.staticDir.empty()) {
    CROW_ROUTE(app, "/<path>")
    ([staticDir = benchCfg.staticDir](const std::string& filePath) {
      std::filesystem::path fullPath = std::filesystem::path(staticDir) / filePath;

      if (!std::filesystem::exists(fullPath) || !std::filesystem::is_regular_file(fullPath)) {
        return crow::response(404, "Not Found");
      }

      std::string content = ReadFile(fullPath);
      crow::response res(200);
      res.add_header("Content-Type", GetContentType(filePath));
      res.body = std::move(content);
      return res;
    });
  }

  // ============================================================
  // Endpoint 10: /r{N} - Routing stress test (literal routes)
  // ============================================================
  // Note: Crow doesn't support dynamic route registration at runtime the same way
  // Drogon does. For routing stress test, we use a pattern-based catch-all approach.
  // This is done via a catch-all handler that parses the route number.

  // Pattern routes for routing stress
  CROW_ROUTE(app, "/users/<string>/posts/<string>")
  ([](const std::string& userId, const std::string& postId) { return std::format("user {} post {}", userId, postId); });

  CROW_ROUTE(app, "/api/v1/resources/<string>/items/<string>/actions/<string>")
  ([](const std::string& resource, const std::string& item, const std::string& action) {
    return std::format("resource {} item {} action {}", resource, item, action);
  });

  // For routing stress test with /r{N} routes, use a regex-based catchall
  // Note: Crow doesn't have native dynamic route generation, so we handle /r<int> pattern
  CROW_ROUTE(app, "/r<int>")
  ([routeCount = benchCfg.routeCount](int routeIdx) {
    if (routeIdx >= 0 && routeIdx < routeCount) {
      return crow::response(200, std::format("route-{}", routeIdx));
    }
    return crow::response(404, "Not Found");
  });

  std::cout << "Crow benchmark server starting on port " << benchCfg.port << " with " << benchCfg.numThreads
            << " threads\n";
  if (!benchCfg.staticDir.empty()) {
    std::cout << "Static files: " << benchCfg.staticDir << "\n";
  }
  if (benchCfg.routeCount > 0) {
    std::cout << "Routes: " << benchCfg.routeCount << " literal + pattern routes\n";
  }
  std::cout << "Server running. Press Ctrl+C to stop.\n";

  app.port(benchCfg.port).concurrency(static_cast<uint16_t>(benchCfg.numThreads)).run();

  return 0;
}
