#include <gtest/gtest.h>
#include <zlib.h>

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "aeronet/http-constants.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/request-decompression-config.hpp"
#include "simple-charconv.hpp"
#include "test_http_client.hpp"
#include "test_server_fixture.hpp"
#ifdef AERONET_ENABLE_ZLIB
#include "zlib-encoder.hpp"
#endif
#ifdef AERONET_ENABLE_ZSTD
#include "zstd-encoder.hpp"
#endif

using namespace aeronet;

namespace {

std::string gzipCompress([[maybe_unused]] std::string_view input) {
#ifdef AERONET_ENABLE_ZLIB
  CompressionConfig cc;  // defaults; level taken from cfg.zlib.level
  ZlibEncoder encoder(details::ZStreamRAII::Variant::gzip, cc);
  return std::string(encoder.encodeFull(input));
#else
  throw std::runtime_error("zlib not enabled");
#endif
}

std::string deflateCompress([[maybe_unused]] std::string_view input) {
#ifdef AERONET_ENABLE_ZLIB
  CompressionConfig cc;
  ZlibEncoder encoder(details::ZStreamRAII::Variant::deflate, cc);
  return std::string(encoder.encodeFull(input));
#else
  throw std::runtime_error("zlib not enabled");
#endif
}

std::string zstdCompress([[maybe_unused]] std::string_view input) {
#ifdef AERONET_ENABLE_ZSTD
  CompressionConfig cc;  // zstd tuning from default config
  ZstdEncoder zencoder(cc);
  return std::string(zencoder.encodeFull(input));
#else
  throw std::runtime_error("zstd not enabled");
#endif
}

struct ClientRawResponse {
  int status{};
  std::string body;
  std::string headersRaw;
};

ClientRawResponse rawPost(uint16_t port, const std::string& target,
                          const std::vector<std::pair<std::string, std::string>>& headers, const std::string& body) {
  test_http_client::RequestOptions opt;
  opt.method = http::POST;
  opt.target = target;
  opt.connection = http::close;
  opt.body = body;
  opt.headers = headers;
  auto raw = test_http_client::request(port, opt);
  if (!raw) {
    throw std::runtime_error("request failed");
  }
  ClientRawResponse resp;
  auto& str = *raw;
  auto firstSpace = str.find(' ');
  auto secondSpace = str.find(' ', firstSpace + 1);
  resp.status = read3(str.substr(firstSpace + 1, secondSpace - firstSpace - 1).data());
  auto headersEnd = str.find("\r\n\r\n");
  resp.headersRaw = str.substr(0, headersEnd);
  resp.body = str.substr(headersEnd + 4);
  return resp;
}

}  // namespace

TEST(HttpRequestDecompression, SingleGzip) {
#ifdef AERONET_ENABLE_ZLIB
  HttpServerConfig cfg{};
  cfg.withRequestDecompression(RequestDecompressionConfig{});
  TestServer ts(cfg);
  std::string plain = "HelloCompressedWorld";
  ts.server.setHandler([plain](const HttpRequest& req) {
    EXPECT_EQ(req.body(), plain);
    HttpResponse resp;
    resp.body("OK");
    return resp;
  });
  auto comp = gzipCompress(plain);
  auto resp = rawPost(ts.port(), "/g", {{"Content-Encoding", "gzip"}}, comp);
  EXPECT_EQ(resp.status, 200) << resp.headersRaw;
#endif
}

TEST(HttpRequestDecompression, SingleDeflate) {
#ifdef AERONET_ENABLE_ZLIB
  HttpServerConfig cfg{};
  cfg.withRequestDecompression(RequestDecompressionConfig{});
  TestServer ts(cfg);
  std::string plain = std::string(100, 'A');
  ts.server.setHandler([plain](const HttpRequest& req) {
    EXPECT_EQ(req.body(), plain);
    HttpResponse resp;
    resp.body("Z");
    return resp;
  });
  auto comp = deflateCompress(plain);
  auto resp = rawPost(ts.port(), "/d", {{"Content-Encoding", "deflate"}}, comp);
  EXPECT_EQ(resp.status, 200);
#endif
}

TEST(HttpRequestDecompression, SingleZstd) {
#ifdef AERONET_ENABLE_ZSTD
  HttpServerConfig cfg{};
  cfg.withRequestDecompression(RequestDecompressionConfig{});
  TestServer ts(cfg);
  std::string plain = std::string(256, 'Z');
  ts.server.setHandler([plain](const HttpRequest& req) {
    EXPECT_EQ(req.body(), plain);
    HttpResponse resp;
    resp.body("S");
    return resp;
  });
  auto comp = zstdCompress(plain);
  auto resp = rawPost(ts.port(), "/z", {{"Content-Encoding", "zstd"}}, comp);
  EXPECT_EQ(resp.status, 200);
#endif
}

TEST(HttpRequestDecompression, MultiGzipDeflateNoSpaces) {
#ifdef AERONET_ENABLE_ZLIB
  HttpServerConfig cfg{};
  cfg.withRequestDecompression(RequestDecompressionConfig{});
  TestServer ts(cfg);
  std::string plain = "MultiStagePayload";
  ts.server.setHandler([plain](const HttpRequest& req) {
    EXPECT_EQ(req.body(), plain);
    HttpResponse resp;
    resp.body("M");
    return resp;
  });
  auto deflated = deflateCompress(plain);
  auto gzipped = gzipCompress(deflated);
  auto resp = rawPost(ts.port(), "/m1", {{"Content-Encoding", "deflate,gzip"}}, gzipped);
  EXPECT_EQ(resp.status, 200);
#endif
}

TEST(HttpRequestDecompression, MultiZstdGzipWithSpaces) {
#if defined(AERONET_ENABLE_ZSTD) && defined(AERONET_ENABLE_ZLIB)
  HttpServerConfig cfg{};
  cfg.withRequestDecompression(RequestDecompressionConfig{});
  TestServer ts(cfg);
  std::string plain = std::string(64, 'Q');
  ts.server.setHandler([plain](const HttpRequest& req) {
    EXPECT_EQ(req.body(), plain);
    HttpResponse resp;
    resp.body("M2");
    return resp;
  });
  auto gz = gzipCompress(plain);
  auto zstd = zstdCompress(gz);
  auto resp = rawPost(ts.port(), "/m2", {{"Content-Encoding", "gzip, zstd"}}, zstd);
  EXPECT_EQ(resp.status, 200);
#endif
}

TEST(HttpRequestDecompression, IdentitySkippedInChain) {
#ifdef AERONET_ENABLE_ZLIB
  HttpServerConfig cfg{};
  cfg.withRequestDecompression(RequestDecompressionConfig{});
  TestServer ts(cfg);
  std::string plain = "SkipIdentity";
  ts.server.setHandler([plain](const HttpRequest& req) {
    EXPECT_EQ(req.body(), plain);
    HttpResponse resp;
    resp.body("I");
    return resp;
  });
  auto deflated = deflateCompress(plain);
  auto gzipped = gzipCompress(deflated);
  auto resp = rawPost(ts.port(), "/i", {{"Content-Encoding", "deflate, identity, gzip"}}, gzipped);
  EXPECT_EQ(resp.status, 200);
#endif
}

TEST(HttpRequestDecompression, UnknownCodingRejected) {
  HttpServerConfig cfg{};
  cfg.withRequestDecompression(RequestDecompressionConfig{});
  TestServer ts(cfg);
  std::string body = "abc";  // not used
  auto resp = rawPost(ts.port(), "/u", {{"Content-Encoding", "br"}}, body);
  EXPECT_EQ(resp.status, http::StatusCodeUnsupportedMediaType);
}

TEST(HttpRequestDecompression, EmptyTokenRejected) {
  HttpServerConfig cfg{};
  cfg.withRequestDecompression(RequestDecompressionConfig{});
  TestServer ts(cfg);
  std::string body = "xyz";
  auto resp = rawPost(ts.port(), "/e", {{"Content-Encoding", "gzip,,deflate"}}, body);
  // gzip compression not actually applied; server will see empty token -> 400
  EXPECT_EQ(resp.status, 400);
}

TEST(HttpRequestDecompression, DisabledFeaturePassThrough) {
#ifdef AERONET_ENABLE_ZLIB
  HttpServerConfig cfg{};
  RequestDecompressionConfig rdc;
  rdc.enable = false;  // disable auto decompression
  cfg.withRequestDecompression(rdc);
  TestServer ts(cfg);
  std::string plain = "ABC";
  auto gz = gzipCompress(plain);
  ts.server.setHandler([](const HttpRequest& req) {
    // With feature disabled the body should still be compressed (gzip header 0x1f 0x8b)
    EXPECT_GE(req.body().size(), 2U);
    EXPECT_EQ(static_cast<unsigned char>(req.body()[0]), 0x1f);
    EXPECT_EQ(static_cast<unsigned char>(req.body()[1]), 0x8b);
    HttpResponse resp;
    resp.body(std::string(req.body()));  // echo raw compressed bytes back
    return resp;
  });
  auto resp = rawPost(ts.port(), "/ds", {{"Content-Encoding", "gzip"}}, gz);
  EXPECT_EQ(resp.status, 200);
  // Response body should match original compressed payload, not decompressed plain text.
  EXPECT_EQ(resp.body, gz);
  EXPECT_NE(resp.body, plain);
#endif
}

TEST(HttpRequestDecompression, ExpansionRatioGuard) {
#ifdef AERONET_ENABLE_ZLIB
  HttpServerConfig cfg{};
  RequestDecompressionConfig rdc;
  rdc.maxExpansionRatio = 2.0;
  rdc.maxDecompressedBytes = 100000;
  cfg.withRequestDecompression(rdc);
  TestServer ts(cfg);
  // Highly compressible large input -> gzip then send; expect rejection if ratio >2
  std::string large(5000, 'A');
  auto gz = gzipCompress(large);
  // Ensure it actually compresses well
  ASSERT_LT(gz.size() * 2, large.size());
  auto resp = rawPost(ts.port(), "/rg", {{"Content-Encoding", "gzip"}}, gz);
  EXPECT_EQ(resp.status, 413);
#endif
}

// ---------------- Additional whitespace / casing / edge chain tests ----------------

TEST(HttpRequestDecompression, MultiZstdGzipMultiSpaces) {
#if defined(AERONET_ENABLE_ZSTD) && defined(AERONET_ENABLE_ZLIB)
  HttpServerConfig cfg{};
  cfg.withRequestDecompression(RequestDecompressionConfig{});
  TestServer ts(cfg);
  std::string plain = std::string(32, 'S');
  ts.server.setHandler([plain](const HttpRequest& req) {
    EXPECT_EQ(req.body(), plain);
    HttpResponse resp;
    resp.body("OK");
    return resp;
  });
  auto gz = gzipCompress(plain);  // first stage
  auto zstd = zstdCompress(gz);   // second stage (listed last in header)
  auto resp = rawPost(ts.port(), "/mspaces", {{"Content-Encoding", "gzip,   zstd"}}, zstd);
  EXPECT_EQ(resp.status, 200);
#endif
}

TEST(HttpRequestDecompression, TripleChainSpacesTabs) {
#if defined(AERONET_ENABLE_ZSTD) && defined(AERONET_ENABLE_ZLIB)
  HttpServerConfig cfg{};
  cfg.withRequestDecompression(RequestDecompressionConfig{});
  TestServer ts(cfg);
  std::string plain = "TripleChain";
  ts.server.setHandler([plain](const HttpRequest& req) {
    EXPECT_EQ(req.body(), plain);
    HttpResponse resp;
    resp.body("T");
    return resp;
  });
  auto d1 = deflateCompress(plain);  // applied first
  auto g2 = gzipCompress(d1);        // applied second
  auto z3 = zstdCompress(g2);        // applied third (header last token)
  auto resp = rawPost(ts.port(), "/triple", {{"Content-Encoding", "deflate,  gzip,\t zstd"}}, z3);
  EXPECT_EQ(resp.status, 200);
#endif
}

TEST(HttpRequestDecompression, MixedCaseTokens) {
#ifdef AERONET_ENABLE_ZLIB
  HttpServerConfig cfg{};
  cfg.withRequestDecompression(RequestDecompressionConfig{});
  TestServer ts(cfg);
  std::string plain = "CaseCheck";
  ts.server.setHandler([plain](const HttpRequest& req) {
    EXPECT_EQ(req.body(), plain);
    HttpResponse resp;
    resp.body("C");
    return resp;
  });
  auto defl = deflateCompress(plain);  // first (leftmost)
  auto gz = gzipCompress(defl);        // second (rightmost)
  auto resp = rawPost(ts.port(), "/case", {{"Content-Encoding", "deflate, GZip"}}, gz);
  EXPECT_EQ(resp.status, 200);
#endif
}

TEST(HttpRequestDecompression, IdentityRepeated) {
#ifdef AERONET_ENABLE_ZLIB
  HttpServerConfig cfg{};
  cfg.withRequestDecompression(RequestDecompressionConfig{});
  TestServer ts(cfg);
  std::string plain = "IdentityRepeat";
  ts.server.setHandler([plain](const HttpRequest& req) {
    EXPECT_EQ(req.body(), plain);
    HttpResponse resp;
    resp.body("IR");
    return resp;
  });
  auto defl = deflateCompress(plain);
  auto gz = gzipCompress(defl);
  auto resp = rawPost(ts.port(), "/idrep", {{"Content-Encoding", "deflate, identity, gzip, identity"}}, gz);
  EXPECT_EQ(resp.status, 200);
#endif
}

TEST(HttpRequestDecompression, TabsBetweenTokens) {
#ifdef AERONET_ENABLE_ZLIB
  HttpServerConfig cfg{};
  cfg.withRequestDecompression(RequestDecompressionConfig{});
  TestServer ts(cfg);
  std::string plain = "TabsBetween";
  ts.server.setHandler([plain](const HttpRequest& req) {
    EXPECT_EQ(req.body(), plain);
    HttpResponse resp;
    resp.body("TB");
    return resp;
  });
  auto defl = deflateCompress(plain);
  auto gz = gzipCompress(defl);
  auto resp = rawPost(ts.port(), "/tabs", {{"Content-Encoding", "deflate,\tgzip"}}, gz);
  EXPECT_EQ(resp.status, 200);
#endif
}

TEST(HttpRequestDecompression, UnknownCodingWithSpacesRejected) {
  HttpServerConfig cfg{};
  cfg.withRequestDecompression(RequestDecompressionConfig{});
  TestServer ts(cfg);
  std::string plain = "UnknownSpace";
  // Apply first encoding (gzip) so that unknown (br) would have been last.
#ifdef AERONET_ENABLE_ZLIB
  auto gz = gzipCompress(plain);
  auto resp = rawPost(ts.port(), "/ubr", {{"Content-Encoding", "gzip,  br"}}, gz);
  EXPECT_EQ(resp.status, http::StatusCodeUnsupportedMediaType);
#else
  auto resp = rawPost(ts.port(), "/ubr", {{"Content-Encoding", "gzip,  br"}}, plain);
  // If zlib disabled, gzip not supported anyway -> still 415 acceptable.
  EXPECT_EQ(resp.status, http::StatusCodeUnsupportedMediaType);
#endif
}

TEST(HttpRequestDecompression, EmptyTokenWithSpacesRejected) {
  HttpServerConfig cfg{};
  cfg.withRequestDecompression(RequestDecompressionConfig{});
  TestServer ts(cfg);
  std::string body = "abc123";  // intentionally not compressed
  auto resp = rawPost(ts.port(), "/emptsp", {{"Content-Encoding", "gzip,  ,deflate"}}, body);
  EXPECT_EQ(resp.status, 400);
}

// ---------------- Corruption / truncated frame tests ----------------

TEST(HttpRequestDecompression, CorruptedGzipTruncatedTail) {
#ifdef AERONET_ENABLE_ZLIB
  HttpServerConfig cfg{};
  cfg.withRequestDecompression(RequestDecompressionConfig{});
  TestServer ts(cfg);
  std::string plain = std::string(200, 'G');
  ts.server.setHandler([plain](const HttpRequest& req) {
    EXPECT_EQ(req.body(), plain);
    HttpResponse resp;
    resp.body("OK");
    return resp;
  });
  auto full = gzipCompress(plain);
  ASSERT_GT(full.size(), 12U);
  // Remove trailing bytes (part of CRC/ISIZE) to induce inflate failure.
  auto truncated = full.substr(0, full.size() - 6);
  auto resp = rawPost(ts.port(), "/cgzip", {{"Content-Encoding", "gzip"}}, truncated);
  EXPECT_EQ(resp.status, 400) << "Expected 400 for truncated gzip frame";
#endif
}

TEST(HttpRequestDecompression, CorruptedZstdBadMagic) {
#ifdef AERONET_ENABLE_ZSTD
  HttpServerConfig cfg{};
  cfg.withRequestDecompression(RequestDecompressionConfig{});
  TestServer ts(cfg);
  std::string plain = std::string(512, 'Z');
  ts.server.setHandler([plain](const HttpRequest& req) {
    EXPECT_EQ(req.body(), plain);
    HttpResponse resp;
    resp.body("OK");
    return resp;
  });
  auto full = zstdCompress(plain);
  ASSERT_GE(full.size(), 4U);  // need room for magic number
  std::string corrupted = full;
  // Flip all bits of first byte of magic number via unsigned char to avoid -Wconversion warning
  {
    unsigned char* bytePtr = reinterpret_cast<unsigned char*>(corrupted.data());
    bytePtr[0] ^= 0xFFU;  // corrupt magic (0x28 -> ~0x28)
  }
  auto resp = rawPost(ts.port(), "/czstd", {{"Content-Encoding", "zstd"}}, corrupted);
  EXPECT_EQ(resp.status, 400) << "Expected 400 for corrupted zstd (bad magic)";
#endif
}
