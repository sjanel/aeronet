#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <string>
#include <thread>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server.hpp"
#include "aeronet/test_util.hpp"

// This test only validates that two servers can bind the same port with SO_REUSEPORT enabled
// and accept at least one connection each. It does not attempt to assert load distribution.

TEST(HttpMultiReusePort, TwoServersBindSamePort) {
  aeronet::HttpServer serverA(aeronet::HttpServerConfig{}.withReusePort());
  serverA.router().setDefault([](const aeronet::HttpRequest&) {
    aeronet::HttpResponse resp;
    resp.body("A");
    return resp;
  });

  auto port = serverA.port();

  aeronet::HttpServer serverB(aeronet::HttpServerConfig{}.withPort(port).withReusePort());
  serverB.router().setDefault([](const aeronet::HttpRequest&) {
    aeronet::HttpResponse resp;
    resp.body("B");
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

  std::string resp1 = aeronet::test::simpleGet(port, "/one");
  std::string resp2 = aeronet::test::simpleGet(port, "/two");
  bool hasA = resp1.contains('A') || resp2.contains('A');
  bool hasB = resp1.contains('B') || resp2.contains('B');
  if (!(hasA && hasB)) {
    // try additional connects with small delays to give scheduler chance to pick different acceptors
    for (int i = 0; i < 15 && !(hasA && hasB); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      std::string retryResp = aeronet::test::simpleGet(port, "/retry");
      if (retryResp.contains('A')) {
        hasA = true;
      }
      if (retryResp.contains('B')) {
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
