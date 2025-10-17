#include <gtest/gtest.h>

#include <chrono>
#include <stdexcept>
#include <thread>
#include <utility>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"

// Validates that moving a running HttpServer (move-construction or move-assignment) throws std::runtime_error
// per the documented semantics (moves only allowed while stopped).
// We cannot test with determinism the move constructor throw because we first move construct the fields before checking
// running status, so the moved-from object may be left in a valid but stopped state.

TEST(HttpServer, MoveAssignWhileRunningThrows) {
  aeronet::HttpServerConfig cfg;
  aeronet::HttpServer serverA(cfg);
  aeronet::HttpServer serverB(cfg);
  serverA.router().setDefault([](const aeronet::HttpRequest&) {
    aeronet::HttpResponse resp;
    resp.body("a");
    return resp;
  });
  serverB.router().setDefault([](const aeronet::HttpRequest&) {
    aeronet::HttpResponse resp;
    resp.body("b");
    return resp;
  });
  std::jthread thr([&] { serverA.run(); });
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  while (!serverA.isRunning() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_TRUE(serverA.isRunning());
  EXPECT_THROW({ serverB = std::move(serverA); }, std::runtime_error);
}
