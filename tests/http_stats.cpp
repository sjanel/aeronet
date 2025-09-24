#include <gtest/gtest.h>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/server-config.hpp"
#include "aeronet/server.hpp"
#include "test_http_client.hpp"
#include "test_server_fixture.hpp"

using namespace aeronet;

TEST(HttpStats, BasicCountersIncrement) {
  ServerConfig cfg;
  cfg.withMaxRequestsPerConnection(5);
  TestServer ts(cfg);
  ts.server.setHandler([]([[maybe_unused]] const HttpRequest& req) {
    HttpResponse resp;
    resp.statusCode = 200;
    resp.reason = "OK";
    resp.body = "hello";
    resp.contentType = "text/plain";
    return resp;
  });
  // Single request via throwing helper
  auto resp = test_http_client::request_or_throw(ts.port());
  ASSERT_NE(resp.find("200 OK"), std::string::npos);
  ts.stop();
  auto st = ts.server.stats();
  EXPECT_GT(st.totalBytesQueued, 0U);  // headers+body accounted
  EXPECT_GT(st.totalBytesWrittenImmediate + st.totalBytesWrittenFlush, 0U);
  EXPECT_GE(st.maxConnectionOutboundBuffer, 0U);
  EXPECT_GE(st.flushCycles, 0U);
}
