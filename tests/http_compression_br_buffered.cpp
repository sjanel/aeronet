#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

#include "aeronet/compression-config.hpp"
#include "aeronet/encoding.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "test_response_parsing.hpp"
#include "test_server_fixture.hpp"

using namespace aeronet;
using testutil::doGet;

TEST(HttpCompressionBrotliBuffered, BrAppliedWhenEligible) {
  CompressionConfig cfg;
  cfg.minBytes = 32;
  cfg.preferredFormats = {Encoding::br};
  HttpServerConfig scfg{};
  scfg.withCompression(cfg);
  TestServer ts(std::move(scfg));
  std::string payload(400, 'B');
  ts.server.setHandler([payload](const HttpRequest &) {
    HttpResponse respObj;
    respObj.customHeader("Content-Type", "text/plain");
    respObj.body(payload);
    return respObj;
  });
  auto resp = doGet(ts.port(), "/br1", {{"Accept-Encoding", "br"}});
  EXPECT_EQ(resp.statusCode, 200);
  auto it = resp.headers.find("Content-Encoding");
  ASSERT_NE(it, resp.headers.end());
  EXPECT_EQ(it->second, "br");
  EXPECT_LT(resp.body.size(), payload.size());
}

TEST(HttpCompressionBrotliBuffered, UserContentEncodingIdentityDisablesCompression) {
  CompressionConfig cfg;
  cfg.minBytes = 1;
  cfg.preferredFormats = {Encoding::br};
  HttpServerConfig scfg{};
  scfg.withCompression(cfg);
  TestServer ts(std::move(scfg));
  std::string payload(128, 'U');
  ts.server.setHandler([payload](const HttpRequest &) {
    HttpResponse respObj;
    respObj.customHeader("Content-Type", "text/plain");
    respObj.customHeader("Content-Encoding", "identity");
    respObj.body(payload);
    return respObj;
  });
  auto resp = doGet(ts.port(), "/br2", {{"Accept-Encoding", "br"}});
  EXPECT_EQ(resp.statusCode, 200);
  auto it = resp.headers.find("Content-Encoding");
  ASSERT_NE(it, resp.headers.end());
  EXPECT_EQ(it->second, "identity");
  EXPECT_EQ(resp.body.size(), payload.size());
}

TEST(HttpCompressionBrotliBuffered, BelowThresholdNotCompressed) {
  CompressionConfig cfg;
  cfg.minBytes = 2048;
  cfg.preferredFormats = {Encoding::br};
  HttpServerConfig scfg{};
  scfg.withCompression(cfg);
  TestServer ts(std::move(scfg));
  std::string small(64, 's');
  ts.server.setHandler([small](const HttpRequest &) {
    HttpResponse respObj;
    respObj.body(small);
    return respObj;
  });
  auto resp = doGet(ts.port(), "/br3", {{"Accept-Encoding", "br"}});
  EXPECT_EQ(resp.statusCode, 200);
  EXPECT_EQ(resp.headers.find("Content-Encoding"), resp.headers.end());
  EXPECT_EQ(resp.body.size(), small.size());
}

TEST(HttpCompressionBrotliBuffered, NoAcceptEncodingHeaderStillCompressesDefault) {
  CompressionConfig cfg;
  cfg.minBytes = 16;
  cfg.preferredFormats = {Encoding::br};
  HttpServerConfig scfg{};
  scfg.withCompression(cfg);
  TestServer ts(std::move(scfg));
  std::string payload(180, 'D');
  ts.server.setHandler([payload](const HttpRequest &) {
    HttpResponse respObj;
    respObj.body(payload);
    return respObj;
  });
  auto resp = doGet(ts.port(), "/br4", {});
  EXPECT_EQ(resp.statusCode, 200);
  auto it = resp.headers.find("Content-Encoding");
  if (it != resp.headers.end()) {
    EXPECT_EQ(it->second, "br");
  }
}

TEST(HttpCompressionBrotliBuffered, IdentityForbiddenNoAlternativesReturns406) {
  CompressionConfig cfg;
  cfg.minBytes = 1;
  cfg.preferredFormats = {Encoding::br};
  HttpServerConfig scfg{};
  scfg.withCompression(cfg);
  TestServer ts(std::move(scfg));
  std::string payload(70, 'Q');
  ts.server.setHandler([payload](const HttpRequest &) {
    HttpResponse respObj;
    respObj.body(payload);
    return respObj;
  });
  auto resp = doGet(ts.port(), "/br5", {{"Accept-Encoding", "identity;q=0, snappy;q=0"}});
  EXPECT_EQ(resp.statusCode, 406);
  EXPECT_EQ(resp.body, "No acceptable content-coding available");
}
