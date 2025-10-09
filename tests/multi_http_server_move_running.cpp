#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>
#include <utility>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/multi-http-server.hpp"
#include "test_response_parsing.hpp"

using testutil::simpleGet;

TEST(MultiHttpServer, MoveWhileRunning) {
  aeronet::HttpServerConfig cfg;
  cfg.withReusePort();
  aeronet::MultiHttpServer multi(cfg);
  multi.setHandler([](const aeronet::HttpRequest&) {
    aeronet::HttpResponse resp;
    resp.body("BeforeMove");
    return resp;
  });
  multi.start();
  auto port = multi.port();
  ASSERT_GT(port, 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  auto resp1 = simpleGet(port, "/pre", {});
  ASSERT_EQ(resp1.statusCode, 200);
  ASSERT_NE(std::string::npos, resp1.body.find("BeforeMove"));

  // Move the running server
  aeronet::MultiHttpServer moved(std::move(multi));
  // After move we still should be able to serve
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  auto resp2 = simpleGet(port, "/post", {});
  EXPECT_EQ(resp2.statusCode, 200);
  EXPECT_NE(std::string::npos, resp2.body.find("BeforeMove"));
  moved.stop();
}

TEST(MultiHttpServer, MoveAssignmentWhileRunning) {
  aeronet::HttpServerConfig cfgA;
  cfgA.port = 0;
  cfgA.withReusePort();
  aeronet::HttpServerConfig cfgB;
  cfgB.port = 0;
  cfgB.withReusePort();
  // Source server
  aeronet::MultiHttpServer src(cfgA);
  src.setHandler([](const aeronet::HttpRequest&) {
    aeronet::HttpResponse resp;
    resp.body("SrcBody");
    return resp;
  });
  src.start();
  auto srcPort = src.port();
  ASSERT_GT(srcPort, 0);
  // Destination server already running with a different body
  aeronet::MultiHttpServer dst(cfgB);
  dst.setHandler([](const aeronet::HttpRequest&) {
    aeronet::HttpResponse resp;
    resp.body("DstOriginal");
    return resp;
  });
  dst.start();
  auto dstPort = dst.port();
  ASSERT_GT(dstPort, 0);
  ASSERT_NE(srcPort, dstPort) << "Ephemeral ports unexpectedly collided";
  std::this_thread::sleep_for(std::chrono::milliseconds(25));
  // Sanity: both respond with their respective bodies
  auto preSrc = simpleGet(srcPort, "/preSrc", {});
  auto preDst = simpleGet(dstPort, "/preDst", {});
  ASSERT_NE(std::string::npos, preSrc.body.find("SrcBody"));
  ASSERT_NE(std::string::npos, preDst.body.find("DstOriginal"));

  // Move-assign: destination adopts source's running threads/servers; its previous threads are stopped inside operator=
  dst = std::move(src);

  // After assignment, dst should serve former source content on source port; old dst port should be inert.
  auto adoptedPort = dst.port();
  EXPECT_EQ(adoptedPort, srcPort);
  std::this_thread::sleep_for(std::chrono::milliseconds(25));
  auto post = simpleGet(adoptedPort, "/after", {});
  EXPECT_NE(std::string::npos, post.body.find("SrcBody"));

  dst.stop();
}