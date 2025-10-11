#include <gtest/gtest.h>

#include <string>

#include "aeronet/http-constants.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_util.hpp"

TEST(HttpStats, BasicCountersIncrement) {
  aeronet::HttpServerConfig cfg;
  cfg.withMaxRequestsPerConnection(5);
  aeronet::test::TestServer ts(cfg);
  ts.server.router().setDefault([]([[maybe_unused]] const aeronet::HttpRequest& req) {
    return aeronet::HttpResponse(200, "OK").body("hello").contentType(aeronet::http::ContentTypeTextPlain);
  });
  // Single request via throwing helper
  auto resp = aeronet::test::requestOrThrow(ts.port());
  ASSERT_NE(resp.find("200 OK"), std::string::npos);
  ts.stop();
  auto st = ts.server.stats();
  EXPECT_GT(st.totalBytesQueued, 0U);  // headers+body accounted
  EXPECT_GT(st.totalBytesWrittenImmediate + st.totalBytesWrittenFlush, 0U);
  EXPECT_GE(st.maxConnectionOutboundBuffer, 0U);
  EXPECT_GE(st.flushCycles, 0U);
}
