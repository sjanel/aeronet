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
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_util.hpp"
#include "zstd_test_helpers.hpp"

using namespace aeronet;

namespace {
bool HasZstdMagic(std::string_view body) {
  // zstd frame magic little endian 0x28 B5 2F FD
  return body.size() >= 4 && static_cast<unsigned char>(body[0]) == 0x28 &&
         static_cast<unsigned char>(body[1]) == 0xB5 && static_cast<unsigned char>(body[2]) == 0x2F &&
         static_cast<unsigned char>(body[3]) == 0xFD;
}
}  // namespace

TEST(HttpCompressionZstdBuffered, ZstdAppliedWhenEligible) {
  CompressionConfig cfg;
  cfg.minBytes = 32;
  cfg.preferredFormats.push_back(Encoding::zstd);
  HttpServerConfig scfg{};
  scfg.withCompression(cfg);
  aeronet::test::TestServer ts(std::move(scfg));
  std::string payload(400, 'A');
  ts.server.setHandler([payload](const HttpRequest&) {
    HttpResponse resp;
    resp.customHeader("Content-Type", "text/plain");
    resp.body(payload);
    return resp;
  });
  auto resp = aeronet::test::simpleGet(ts.port(), "/z", {{"Accept-Encoding", "zstd"}});
  ASSERT_EQ(resp.statusCode, 200);
  auto it = resp.headers.find("Content-Encoding");
  ASSERT_NE(it, resp.headers.end());
  EXPECT_EQ(it->second, "zstd");
  EXPECT_TRUE(HasZstdMagic(resp.body));
  EXPECT_LT(resp.body.size(), payload.size());
  // Round-trip verify by decompressing (simple one-shot) to ensure integrity
  std::string decompressed = aeronet::test::zstdRoundTripDecompress(resp.body, payload.size());
  EXPECT_EQ(decompressed, payload);
}

TEST(HttpCompressionZstdBuffered, WildcardSelectsZstdIfPreferred) {
  CompressionConfig cfg;
  cfg.minBytes = 16;
  cfg.preferredFormats.push_back(Encoding::zstd);
  cfg.preferredFormats.push_back(Encoding::gzip);
  HttpServerConfig scfg{};
  scfg.withCompression(cfg);
  aeronet::test::TestServer ts(std::move(scfg));
  std::string payload(256, 'B');
  ts.server.setHandler([payload](const HttpRequest&) {
    HttpResponse resp;
    resp.body(payload);
    resp.customHeader("Content-Type", "text/plain");
    return resp;
  });
  auto resp = aeronet::test::simpleGet(ts.port(), "/w", {{"Accept-Encoding", "*;q=0.9"}});
  auto it = resp.headers.find("Content-Encoding");
  ASSERT_NE(it, resp.headers.end());
  EXPECT_EQ(it->second, "zstd");
  EXPECT_TRUE(HasZstdMagic(resp.body));
}

TEST(HttpCompressionZstdBuffered, TieBreakAgainstGzipHigherQ) {
  CompressionConfig cfg;
  cfg.minBytes = 16;
  cfg.preferredFormats.push_back(Encoding::zstd);
  cfg.preferredFormats.push_back(Encoding::gzip);
  HttpServerConfig scfg{};
  scfg.withCompression(cfg);
  aeronet::test::TestServer ts(std::move(scfg));
  std::string payload(512, 'C');
  ts.server.setHandler([payload](const HttpRequest&) {
    HttpResponse resp;
    resp.body(payload);
    resp.customHeader("Content-Type", "text/plain");
    return resp;
  });
  auto resp = aeronet::test::simpleGet(ts.port(), "/t", {{"Accept-Encoding", "gzip;q=0.9, zstd;q=0.9"}});
  auto it = resp.headers.find("Content-Encoding");
  ASSERT_NE(it, resp.headers.end());
  EXPECT_EQ(it->second, "zstd");
}
