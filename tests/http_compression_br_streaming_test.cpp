#include <gtest/gtest.h>

#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aeronet/compression-config.hpp"
#include "aeronet/encoding.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response-writer.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_util.hpp"

using namespace aeronet;

TEST(HttpCompressionBrotliStreaming, BrActivatedOverThreshold) {
  CompressionConfig cfg;
  cfg.minBytes = 64;
  cfg.preferredFormats = {Encoding::br};
  HttpServerConfig scfg{};
  scfg.withCompression(cfg);
  aeronet::test::TestServer ts(std::move(scfg));
  std::string part1(40, 'a');
  std::string part2(80, 'b');
  ts.server.router().setDefault([&]([[maybe_unused]] const HttpRequest &req, HttpResponseWriter &writer) {
    writer.statusCode(200);
    writer.contentType("text/plain");
    writer.writeBody(part1);
    writer.writeBody(part2);
    writer.end();
  });
  auto resp = aeronet::test::simpleGet(ts.port(), "/sbr1", {{"Accept-Encoding", "br"}});
  auto it = resp.headers.find("Content-Encoding");
  if (it != resp.headers.end()) {
    EXPECT_EQ(it->second, "br");
  }
  // Size heuristic: compressed should be smaller than concatenated plain text.
  EXPECT_LT(resp.body.size(), part1.size() + part2.size());
}

TEST(HttpCompressionBrotliStreaming, BelowThresholdIdentity) {
  CompressionConfig cfg;
  cfg.minBytes = 1024;
  cfg.preferredFormats = {Encoding::br};
  HttpServerConfig scfg{};
  scfg.withCompression(cfg);
  aeronet::test::TestServer ts(std::move(scfg));
  std::string small(80, 'x');
  ts.server.router().setDefault([&]([[maybe_unused]] const HttpRequest &req, HttpResponseWriter &writer) {
    writer.statusCode(200);
    writer.contentType("text/plain");
    writer.writeBody(small);
    writer.end();
  });
  auto resp = aeronet::test::simpleGet(ts.port(), "/sbr2", {{"Accept-Encoding", "br"}});
  EXPECT_EQ(resp.headers.find("Content-Encoding"), resp.headers.end());
  EXPECT_TRUE(resp.body.contains('x'));
}

TEST(HttpCompressionBrotliStreaming, UserProvidedIdentityPreventsActivation) {
  CompressionConfig cfg;
  cfg.minBytes = 16;
  cfg.preferredFormats = {Encoding::br};
  HttpServerConfig scfg{};
  scfg.withCompression(cfg);
  aeronet::test::TestServer ts(std::move(scfg));
  std::string payload(512, 'Y');
  ts.server.router().setDefault([&]([[maybe_unused]] const HttpRequest &req, HttpResponseWriter &writer) {
    writer.statusCode(200);
    writer.customHeader("Content-Encoding", "identity");
    writer.writeBody(payload);
    writer.end();
  });
  auto resp = aeronet::test::simpleGet(ts.port(), "/sbr3", {{"Accept-Encoding", "br"}});
  auto it = resp.headers.find("Content-Encoding");
  ASSERT_NE(it, resp.headers.end());
  EXPECT_EQ(it->second, "identity");
  // Streaming identity may use chunked transfer, so body size can exceed raw payload due to framing; just ensure
  // we did not apply brotli (which would eliminate long runs of 'Y').
  EXPECT_TRUE(resp.body.contains(std::string(32, 'Y')));
}

TEST(HttpCompressionBrotliStreaming, QValuesInfluenceSelection) {
  CompressionConfig cfg;
  cfg.minBytes = 64;
  cfg.preferredFormats = {Encoding::gzip, Encoding::br};  // preferences list order
  HttpServerConfig scfg{};
  scfg.withCompression(cfg);
  aeronet::test::TestServer ts(std::move(scfg));
  std::string payload(600, 'Z');
  ts.server.router().setDefault([&]([[maybe_unused]] const HttpRequest &req, HttpResponseWriter &writer) {
    writer.statusCode(200);
    writer.contentType("text/plain");
    writer.writeBody(payload.substr(0, 128));
    writer.writeBody(payload.substr(128));
    writer.end();
  });
  // Client strongly prefers br
  auto resp = aeronet::test::simpleGet(ts.port(), "/sbr4", {{"Accept-Encoding", "gzip;q=0.5, br;q=1.0"}});
  auto it = resp.headers.find("Content-Encoding");
  if (it != resp.headers.end()) {
    EXPECT_EQ(it->second, "br");
  }
}

TEST(HttpCompressionBrotliStreaming, IdentityForbiddenNoAlternativesReturns406) {
  CompressionConfig cfg;
  cfg.minBytes = 1;
  cfg.preferredFormats = {Encoding::br};
  HttpServerConfig scfg{};
  scfg.withCompression(cfg);
  aeronet::test::TestServer ts(std::move(scfg));
  std::string payload(90, 'F');
  ts.server.router().setDefault([&]([[maybe_unused]] const HttpRequest &req, HttpResponseWriter &writer) {
    writer.statusCode(200);
    writer.writeBody(payload);
    writer.end();
  });
  auto resp = aeronet::test::simpleGet(ts.port(), "/sbr5", {{"Accept-Encoding", "identity;q=0, snappy;q=0"}});
  // Server should respond 406 (not compressible with offered encodings; identity forbidden)
  EXPECT_TRUE(resp.headersRaw.contains(" 406 "));
}
