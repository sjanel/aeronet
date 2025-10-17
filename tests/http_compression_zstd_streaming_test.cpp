#include <gtest/gtest.h>
#include <zstd.h>

#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aeronet/compression-config.hpp"
#include "aeronet/encoding.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response-writer.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_util.hpp"
#include "zstd_test_helpers.hpp"

using namespace aeronet;

TEST(HttpCompressionZstdStreaming, ZstdActivatesAfterThreshold) {
  CompressionConfig cfg;
  cfg.minBytes = 128;
  cfg.preferredFormats.push_back(Encoding::zstd);
  HttpServerConfig scfg{};
  scfg.withCompression(cfg);
  aeronet::test::TestServer ts(std::move(scfg));
  std::string chunk1(64, 'x');
  std::string chunk2(128, 'y');
  ts.server.router().setDefault([&](const HttpRequest &, HttpResponseWriter &writer) {
    writer.statusCode(200);
    writer.contentType("text/plain");
    writer.writeBody(chunk1);
    writer.writeBody(chunk2);
    writer.end();
  });
  auto resp = test::simpleGet(ts.port(), "/zs", {{"Accept-Encoding", "zstd"}});
  auto it = resp.headers.find("Content-Encoding");
  ASSERT_NE(it, resp.headers.end());
  EXPECT_EQ(it->second, "zstd");
  EXPECT_TRUE(test::HasZstdMagic(resp.plainBody));
  // Round-trip decompression via helper
  std::string original = chunk1 + chunk2;
  auto decompressed = aeronet::test::zstdRoundTripDecompress(resp.plainBody, original.size());
  EXPECT_EQ(decompressed, original);
}

TEST(HttpCompressionZstdStreaming, BelowThresholdIdentity) {
  CompressionConfig cfg;
  cfg.minBytes = 1024;
  cfg.preferredFormats.push_back(Encoding::zstd);
  HttpServerConfig scfg{};
  scfg.withCompression(cfg);
  aeronet::test::TestServer ts(std::move(scfg));
  std::string data(200, 'a');
  ts.server.router().setDefault([&](const HttpRequest &, HttpResponseWriter &writer) {
    writer.statusCode(200);
    writer.contentType("text/plain");
    writer.writeBody(data);
    writer.end();
  });
  auto resp = test::simpleGet(ts.port(), "/zi", {{"Accept-Encoding", "zstd"}});
  auto it = resp.headers.find("Content-Encoding");
  EXPECT_TRUE(it == resp.headers.end());  // identity
  EXPECT_TRUE(resp.plainBody == data) << "identity path should match input exactly";
}
