// Reuse common test helpers instead of ad-hoc socket code.
#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>

#include "aeronet/http-method.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "aeronet/test_util.hpp"

TEST(HttpQueryStructuredBindings, IterateKeyValues) {
  aeronet::HttpServer server(aeronet::HttpServerConfig{});
  server.router().setPath("/sb", aeronet::http::Method::GET,
                          [](const aeronet::HttpRequest& req) -> aeronet::HttpResponse {
                            EXPECT_EQ(req.path(), "/sb");
                            int count = 0;
                            bool sawA = false;
                            bool sawB = false;
                            bool sawEmpty = false;
                            bool sawNoValue = false;
                            for (const auto& [k, v] : req.queryParams()) {
                              ++count;
                              if (k == "a") {
                                EXPECT_EQ(v, "1");
                                sawA = true;
                              } else if (k == "b") {
                                EXPECT_EQ(v, "two words");
                                sawB = true;
                              } else if (k == "empty") {
                                EXPECT_TRUE(v.empty());
                                sawEmpty = true;
                              } else if (k == "novalue") {
                                EXPECT_TRUE(v.empty());
                                sawNoValue = true;
                              }
                            }
                            EXPECT_EQ(count, 4);
                            EXPECT_TRUE(sawA && sawB && sawEmpty && sawNoValue);
                            aeronet::HttpResponse resp(200, "OK");
                            resp.body("OK").contentType("text/plain");
                            return resp;
                          });
  std::jthread thr([&] { server.run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  // Build raw HTTP request using helpers
  aeronet::test::ClientConnection client(server.port());
  std::string req = "GET /sb?a=1&b=two%20words&empty=&novalue HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n";
  ASSERT_TRUE(aeronet::test::sendAll(client.fd(), req));
  auto resp = aeronet::test::recvUntilClosed(client.fd());
  EXPECT_NE(resp.find("OK"), std::string::npos);
  server.stop();
}
