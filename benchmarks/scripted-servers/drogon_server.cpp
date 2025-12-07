// drogon_server.cpp - Drogon benchmark server for wrk testing
//
// Implements the same endpoints as aeronet_server.cpp for fair comparison.
// Requires AERONET_BENCH_ENABLE_DROGON=ON during CMake configuration.

#include <drogon/drogon.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <string_view>
#include <thread>

#include "drogon/HttpTypes.h"

namespace {

constexpr unsigned char toupper(unsigned char ch) {
  if (ch >= 'a' && ch <= 'z') {
    ch &= 0xDF;  // clear lowercase bit
  }
  return ch;
}

constexpr char toupper(char ch) { return static_cast<char>(toupper(static_cast<unsigned char>(ch))); }

// CPU-bound computation for /compute endpoint
uint64_t Fibonacci(int n) {
  if (n <= 1) {
    return static_cast<uint64_t>(n);
  }
  uint64_t prev = 0;
  uint64_t curr = 1;
  for (int i = 2; i <= n; ++i) {
    uint64_t next = prev + curr;
    prev = curr;
    curr = next;
  }
  return curr;
}

// Simple hash computation for CPU stress
uint64_t ComputeHash(std::string_view data, int iterations) {
  uint64_t hash = 0xcbf29ce484222325ULL;  // FNV-1a offset basis
  for (int iter = 0; iter < iterations; ++iter) {
    for (char signedCh : data) {
      auto ch = static_cast<unsigned char>(signedCh);
      hash ^= ch;
      hash *= 0x100000001b3ULL;  // FNV-1a prime
    }
  }
  return hash;
}

// Generate random string for response bodies
std::string GenerateRandomString(std::size_t length) {
  static constexpr char kCharset[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
  static thread_local std::mt19937_64 rng(std::random_device{}());
  std::uniform_int_distribution<std::size_t> dist(0, sizeof(kCharset) - 2);

  std::string result(length, '\0');
  for (std::size_t pos = 0; pos < length; ++pos) {
    result[pos] = kCharset[dist(rng)];
  }
  return result;
}

int GetNumThreads() {
  const char* envThreads = std::getenv("BENCH_THREADS");
  if (envThreads != nullptr) {
    return std::atoi(envThreads);
  }
  int hwThreads = static_cast<int>(std::thread::hardware_concurrency());
  return std::max(1, hwThreads / 2);
}

uint16_t GetPort() {
  const char* envPort = std::getenv("BENCH_PORT");
  if (envPort != nullptr) {
    return static_cast<uint16_t>(std::atoi(envPort));
  }
  return 8081;  // Different default port than aeronet
}

drogon::ContentType GetContentType(std::string_view path) {
  if (path.ends_with(".html")) {
    return drogon::ContentType::CT_TEXT_HTML;
  }
  if (path.ends_with(".css")) {
    return drogon::ContentType::CT_TEXT_CSS;
  }
  if (path.ends_with(".js")) {
    return drogon::ContentType::CT_TEXT_JAVASCRIPT;
  }
  if (path.ends_with(".json")) {
    return drogon::ContentType::CT_APPLICATION_JSON;
  }
  return drogon::ContentType::CT_APPLICATION_OCTET_STREAM;
}

}  // namespace

int main(int argc, char* argv[]) {
  uint16_t port = GetPort();
  int numThreads = GetNumThreads();
  std::string staticDir;
  int routeCount = 0;

  for (int argPos = 1; argPos < argc; ++argPos) {
    std::string_view arg(argv[argPos]);
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
                << "  --port N      Listen port (default: 8081, env: BENCH_PORT)\n"
                << "  --threads N   Worker threads (default: nproc/2, env: BENCH_THREADS)\n"
                << "  --static DIR  Static files directory\n"
                << "  --routes N    Number of /r{N} routes for routing stress test\n"
                << "  --help        Show this help\n";
      return 0;
    }
  }

  auto& app = drogon::app();

  // Configure Drogon
  app.addListener("127.0.0.1", port);
  app.setThreadNum(static_cast<std::size_t>(numThreads));
  app.setIdleConnectionTimeout(0);               // No timeout for benchmarks
  app.setPipeliningRequestsNumber(1000000);      // Allow many pipelined requests
  app.setClientMaxBodySize(64UL * 1024 * 1024);  // 64MB body limit

  // ============================================================
  // Endpoint 1: /ping - Minimal latency test
  // ============================================================
  app.registerHandler(
      "/ping",
      [](const drogon::HttpRequestPtr&, std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setStatusCode(drogon::k200OK);
        resp->setBody("pong");
        callback(resp);
      },
      {drogon::Get});

  // ============================================================
  // Endpoint 2: /headers - Header stress test
  // ============================================================
  app.registerHandler(
      "/headers",
      [](const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
        std::size_t count = 10;
        std::size_t headerSize = 64;

        auto countParam = req->getParameter("count");
        auto sizeParam = req->getParameter("size");
        if (!countParam.empty()) {
          count = static_cast<std::size_t>(std::stoull(countParam));
        }
        if (!sizeParam.empty()) {
          headerSize = static_cast<std::size_t>(std::stoull(sizeParam));
        }

        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setStatusCode(drogon::k200OK);

        for (std::size_t headerPos = 0; headerPos < count; ++headerPos) {
          resp->addHeader(std::format("X-Bench-Header-{}", headerPos), GenerateRandomString(headerSize));
        }
        resp->setBody(std::format("Generated {} headers", count));
        callback(resp);
      },
      {drogon::Get});

  // ============================================================
  // Endpoint 3: /uppercase - Body uppercase test
  // ============================================================
  app.registerHandler(
      "/uppercase",
      [](const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
        std::string_view body = req->body();
        std::string out;
        out.resize_and_overwrite(body.size(), [body](char* out, std::size_t n) {
          std::ranges::transform(body, out, [](char ch) { return toupper(ch); });
          return n;
        });

        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setStatusCode(drogon::k200OK);
        resp->setBody(std::move(out));
        callback(resp);
      },
      {drogon::Post});

  // ============================================================
  // Endpoint 4: /compute - CPU-bound test
  // ============================================================
  app.registerHandler(
      "/compute",
      [](const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
        int complexity = 30;
        int hashIters = 1000;

        auto complexityParam = req->getParameter("complexity");
        auto hashParam = req->getParameter("hash_iters");
        if (!complexityParam.empty()) {
          complexity = std::stoi(complexityParam);
        }
        if (!hashParam.empty()) {
          hashIters = std::stoi(hashParam);
        }

        uint64_t fibResult = Fibonacci(complexity);
        std::string data = std::format("benchmark-data-{}", complexity);
        uint64_t hashResult = ComputeHash(data, hashIters);

        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setStatusCode(drogon::k200OK);
        resp->addHeader("X-Fib-Result", std::to_string(fibResult));
        resp->addHeader("X-Hash-Result", std::to_string(hashResult));
        resp->setBody(std::format("fib({})={}, hash={}", complexity, fibResult, hashResult));
        callback(resp);
      },
      {drogon::Get});

  // ============================================================
  // Endpoint 5: /json - JSON response test
  // ============================================================
  app.registerHandler(
      "/json",
      [](const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
        std::size_t items = 10;
        auto itemsParam = req->getParameter("items");
        if (!itemsParam.empty()) {
          items = static_cast<std::size_t>(std::stoull(itemsParam));
        }

        std::string json = "{\"items\":[";
        for (std::size_t itemPos = 0; itemPos < items; ++itemPos) {
          if (itemPos > 0) {
            json += ",";
          }
          json += std::format(R"({{"id":{},"name":"item-{}","value":{}}})", itemPos, itemPos, itemPos * 100);
        }
        json += "]}";

        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setStatusCode(drogon::k200OK);
        resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
        resp->setBody(std::move(json));
        callback(resp);
      },
      {drogon::Get});

  // ============================================================
  // Endpoint 6: /delay - Artificial delay test
  // ============================================================
  app.registerHandler(
      "/delay",
      [](const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
        int delayMs = 10;
        auto msParam = req->getParameter("ms");
        if (!msParam.empty()) {
          delayMs = std::stoi(msParam);
        }

        // Use Drogon's async delay mechanism
        auto loop = drogon::app().getLoop();
        loop->runAfter(static_cast<double>(delayMs) / 1000.0, [callback = std::move(callback), delayMs]() {
          auto resp = drogon::HttpResponse::newHttpResponse();
          resp->setStatusCode(drogon::k200OK);
          resp->setBody(std::format("Delayed {} ms", delayMs));
          callback(resp);
        });
      },
      {drogon::Get});

  // ============================================================
  // Endpoint 7: /body - Variable size body test
  // ============================================================
  app.registerHandler(
      "/body",
      [](const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
        std::size_t size = 1024;
        auto sizeParam = req->getParameter("size");
        if (!sizeParam.empty()) {
          size = static_cast<std::size_t>(std::stoull(sizeParam));
        }

        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setStatusCode(drogon::k200OK);
        resp->setBody(GenerateRandomString(size));
        callback(resp);
      },
      {drogon::Get});

  // ============================================================
  // Endpoint 8: /status - Health check
  // ============================================================
  app.registerHandler(
      "/status",
      [numThreads](const drogon::HttpRequestPtr&, std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setStatusCode(drogon::k200OK);
        resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
        resp->setBody(std::format(R"({{"server":"drogon","threads":{},"status":"ok"}})", numThreads));
        callback(resp);
      },
      {drogon::Get});

  // ============================================================
  // Endpoint 9: /* - Static file serving
  // ============================================================
  if (!staticDir.empty()) {
    app.registerHandler(
        "/{file_path}",
        [staticDir]([[maybe_unused]] const drogon::HttpRequestPtr& req,
                    std::function<void(const drogon::HttpResponsePtr&)>&& callback, std::string filePath) {
          std::filesystem::path fullPath = std::filesystem::path(staticDir) / filePath;

          if (!std::filesystem::exists(fullPath) || !std::filesystem::is_regular_file(fullPath)) {
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setStatusCode(drogon::k404NotFound);
            resp->setBody("Not Found");
            callback(resp);
            return;
          }

          static constexpr std::string kAttachmentFileName{};

          auto resp = drogon::HttpResponse::newFileResponse(fullPath, kAttachmentFileName, GetContentType(filePath));
          resp->setStatusCode(drogon::k200OK);
          callback(resp);
        },
        {drogon::Get});
  }

  // ============================================================
  // Endpoint 10: /r{N} - Routing stress test (literal routes)
  // ============================================================
  if (routeCount > 0) {
    for (int routeIdx = 0; routeIdx < routeCount; ++routeIdx) {
      std::string path = std::format("/r{}", routeIdx);
      app.registerHandler(
          path,
          [routeIdx](const drogon::HttpRequestPtr&, std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setStatusCode(drogon::k200OK);
            resp->setBody(std::format("route {}", routeIdx));
            callback(resp);
          },
          {drogon::Get});
    }

    // Pattern routes for routing stress
    app.registerHandler(
        "/users/{user_id}/posts/{post_id}",
        [](const drogon::HttpRequestPtr&, std::function<void(const drogon::HttpResponsePtr&)>&& callback,
           std::string userId, std::string postId) {
          auto resp = drogon::HttpResponse::newHttpResponse();
          resp->setStatusCode(drogon::k200OK);
          resp->setBody(std::format("user {} post {}", userId, postId));
          callback(resp);
        },
        {drogon::Get});

    app.registerHandler(
        "/api/v1/resources/{resource}/items/{item}/actions/{action}",
        [](const drogon::HttpRequestPtr&, std::function<void(const drogon::HttpResponsePtr&)>&& callback,
           std::string resource, std::string item, std::string action) {
          auto resp = drogon::HttpResponse::newHttpResponse();
          resp->setStatusCode(drogon::k200OK);
          resp->setBody(std::format("resource {} item {} action {}", resource, item, action));
          callback(resp);
        },
        {drogon::Get});
  }

  std::cout << "drogon benchmark server starting on port " << port << " with " << numThreads << " threads\n";
  if (!staticDir.empty()) {
    std::cout << "Static files: " << staticDir << "\n";
  }
  if (routeCount > 0) {
    std::cout << "Routes: " << routeCount << " literal + pattern routes\n";
  }
  std::cout << "Server running. Press Ctrl+C to stop.\n";

  app.run();

  return 0;
}
