#include <gtest/gtest.h>

#include <string>  // std::string

#include "aeronet/http-constants.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "test_http_client.hpp"
#include "test_server_fixture.hpp"

using namespace aeronet;

TEST(HttpStats, BasicCountersIncrement) {
  HttpServerConfig cfg;
  cfg.withMaxRequestsPerConnection(5);
  TestServer ts(cfg);
  ts.server.setHandler([]([[maybe_unused]] const HttpRequest& req) {
    return aeronet::HttpResponse(200, "OK").body("hello").contentType(aeronet::http::ContentTypeTextPlain);
  });
  // Single request via throwing helper
  auto resp = test_http_client::requestOrThrow(ts.port());
  ASSERT_NE(resp.find("200 OK"), std::string::npos);
  ts.stop();
  auto st = ts.server.stats();
  EXPECT_GT(st.totalBytesQueued, 0U);  // headers+body accounted
  EXPECT_GT(st.totalBytesWrittenImmediate + st.totalBytesWrittenFlush, 0U);
  EXPECT_GE(st.maxConnectionOutboundBuffer, 0U);
  EXPECT_GE(st.flushCycles, 0U);
}
