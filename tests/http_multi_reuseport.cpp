#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <string>
#include <thread>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server.hpp"
#include "test_raw_get.hpp"

// This test only validates that two servers can bind the same port with SO_REUSEPORT enabled
// and accept at least one connection each. It does not attempt to assert load distribution.

TEST(HttpMultiReusePort, TwoServersBindSamePort) {
  aeronet::HttpServer serverA(aeronet::HttpServerConfig{}.withReusePort());
  serverA.setHandler([](const aeronet::HttpRequest&) {
    aeronet::HttpResponse resp;
    resp.body = "A";
    return resp;
  });

  auto port = serverA.port();

  aeronet::HttpServer serverB(aeronet::HttpServerConfig{}.withPort(port).withReusePort());
  serverB.setHandler([](const aeronet::HttpRequest&) {
    aeronet::HttpResponse resp;
    resp.body = "B";
    return resp;
  });

  std::promise<void> startedA;
  std::promise<void> startedB;

  std::jthread tA([&] {
    startedA.set_value();
    serverA.run();
  });
  startedA.get_future().wait();
  std::jthread tB([&] {
    startedB.set_value();
    serverB.run();
  });
  startedB.get_future().wait();

  // Give kernel a moment to establish both listening sockets
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  std::string resp1;
  std::string resp2;
  test_helpers::rawGet(port, "/one", resp1);
  test_helpers::rawGet(port, "/two", resp2);
  bool hasA = resp1.find('A') != std::string::npos || resp2.find('A') != std::string::npos;
  bool hasB = resp1.find('B') != std::string::npos || resp2.find('B') != std::string::npos;
  if (!(hasA && hasB)) {
    // try additional connects with small delays to give scheduler chance to pick different acceptors
    for (int i = 0; i < 15 && !(hasA && hasB); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      std::string retryResp;
      test_helpers::rawGet(port, "/retry", retryResp);
      if (retryResp.find('A') != std::string::npos) {
        hasA = true;
      }
      if (retryResp.find('B') != std::string::npos) {
        hasB = true;
      }
    }
  }

  serverA.stop();
  serverB.stop();

  // At least one of the responses should contain body A and one body B
  // Because of hashing, both could come from same server but with two sequential connects
  // we expect distribution eventually, so tolerate the rare case of both identical by allowing either pattern
  EXPECT_TRUE(hasA);
  EXPECT_TRUE(hasB);
}
