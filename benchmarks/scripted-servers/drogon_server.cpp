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
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

#include "drogon/HttpTypes.h"
#include "scripted-servers-helpers.hpp"

namespace {

constexpr unsigned char toupper(unsigned char ch) {
  if (ch >= 'a' && ch <= 'z') {
    ch &= 0xDF;  // clear lowercase bit
  }
  return ch;
}

constexpr char toupper(char ch) { return static_cast<char>(toupper(static_cast<unsigned char>(ch))); }

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
  bench::BenchConfig benchCfg(8081, argc, argv);

  auto& app = drogon::app();

  // Configure Drogon
  app.addListener("127.0.0.1", benchCfg.port);
  app.setThreadNum(static_cast<std::size_t>(benchCfg.numThreads));
  app.setIdleConnectionTimeout(0);                      // No timeout for benchmarks
  app.setPipeliningRequestsNumber(1000000000);          // Allow many pipelined requests
  app.setClientMaxBodySize(4ULL * 1024 * 1024 * 1024);  // 4GB body limit

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
          resp->addHeader(std::format("X-Bench-Header-{}", headerPos), bench::GenerateRandomString(headerSize));
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

        uint64_t fibResult = bench::Fibonacci(complexity);
        std::string data = std::format("benchmark-data-{}", complexity);
        uint64_t hashResult = bench::ComputeHash(data, hashIters);

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

        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setStatusCode(drogon::k200OK);
        resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
        resp->setBody(bench::BuildJson(items));
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
        resp->setBody(bench::GenerateRandomString(size));
        callback(resp);
      },
      {drogon::Get});

  // ============================================================
  // Endpoint 7b: /body-codec - Gzip decode/encode stress test
  // ============================================================
  app.registerHandler(
      "/body-codec",
      [](const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
        std::string_view body = req->body();
        auto encoding = req->getHeader("Content-Encoding");
        auto acceptEncoding = req->getHeader("Accept-Encoding");
        if (acceptEncoding.empty() || encoding.empty() || !bench::ContainsTokenInsensitive(encoding, "gzip") ||
            !bench::ContainsTokenInsensitive(acceptEncoding, "gzip")) {
          auto resp = drogon::HttpResponse::newHttpResponse();
          resp->setStatusCode(drogon::k400BadRequest);
          resp->setBody("Invalid gzip request");
          callback(resp);
          return;
        }
        std::string decoded;
        auto decompressed = bench::GzipDecompress(body);
        if (!decompressed) {
          auto resp = drogon::HttpResponse::newHttpResponse();
          resp->setStatusCode(drogon::k400BadRequest);
          resp->setBody("Invalid gzip body");
          callback(resp);
          return;
        }
        decoded = std::move(*decompressed);

        for (char& ch : decoded) {
          ch = static_cast<char>(static_cast<unsigned char>(ch + 1U));
        }

        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setStatusCode(drogon::k200OK);
        resp->setContentTypeCode(drogon::CT_APPLICATION_OCTET_STREAM);

        auto compressed = bench::GzipCompress(decoded);
        if (!compressed) {
          resp->setStatusCode(drogon::k500InternalServerError);
          resp->setBody("Compression failed");
        } else {
          resp->addHeader("Content-Encoding", "gzip");
          resp->addHeader("Vary", "Accept-Encoding");
          resp->setBody(std::move(*compressed));
        }
        callback(resp);
      },
      {drogon::Post});

  // ============================================================
  // Endpoint 8: /status - Health check
  // ============================================================
  app.registerHandler(
      "/status",
      [numThreads = benchCfg.numThreads](const drogon::HttpRequestPtr&,
                                         std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
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
  if (!benchCfg.staticDir.empty()) {
    app.registerHandler("/{file_path}",
                        [staticDir = benchCfg.staticDir]([[maybe_unused]] const drogon::HttpRequestPtr& req,
                                                         std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                                                         std::string filePath) {
                          std::filesystem::path fullPath = std::filesystem::path(staticDir) / filePath;

                          if (!std::filesystem::exists(fullPath) || !std::filesystem::is_regular_file(fullPath)) {
                            auto resp = drogon::HttpResponse::newHttpResponse();
                            resp->setStatusCode(drogon::k404NotFound);
                            resp->setBody("Not Found");
                            callback(resp);
                            return;
                          }

                          static constexpr std::string kAttachmentFileName{};

                          auto resp = drogon::HttpResponse::newFileResponse(fullPath, kAttachmentFileName,
                                                                            GetContentType(filePath));
                          resp->setStatusCode(drogon::k200OK);
                          callback(resp);
                        },
                        {drogon::Get});
  }

  // ============================================================
  // Endpoint 10: /r{N} - Routing stress test (literal routes)
  // ============================================================
  for (int routeIdx = 0; routeIdx < benchCfg.routeCount; ++routeIdx) {
    std::string path = std::format("/r{}", routeIdx);
    app.registerHandler(
        path,
        [routeIdx](const drogon::HttpRequestPtr&, std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
          auto resp = drogon::HttpResponse::newHttpResponse();
          resp->setStatusCode(drogon::k200OK);
          resp->setBody(std::format("route-{}", routeIdx));
          callback(resp);
        },
        {drogon::Get});
  }
  std::cout << "Routes: " << benchCfg.routeCount << " literal + pattern routes\n";
  // Pattern routes for routing stress
  app.registerHandler("/users/{user_id}/posts/{post_id}",
                      [](const drogon::HttpRequestPtr&, std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                         std::string userId, std::string postId) {
                        auto resp = drogon::HttpResponse::newHttpResponse();
                        resp->setStatusCode(drogon::k200OK);
                        resp->setBody(std::format("user {} post {}", userId, postId));
                        callback(resp);
                      },
                      {drogon::Get});

  app.registerHandler("/api/v{version}/items/{item}",
                      [](const drogon::HttpRequestPtr&, std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                         std::string version, std::string item) {
                        auto resp = drogon::HttpResponse::newHttpResponse();
                        resp->setStatusCode(drogon::k200OK);
                        resp->setBody(std::format("version {} item {}", version, item));
                        callback(resp);
                      },
                      {drogon::Get});

  std::cout << "drogon benchmark server starting on port " << benchCfg.port << " with " << benchCfg.numThreads
            << " threads\n";
  if (!benchCfg.staticDir.empty()) {
    std::cout << "Static files: " << benchCfg.staticDir << "\n";
  }
  std::cout << "Server running. Press Ctrl+C to stop.\n";

  app.run();

  return 0;
}
