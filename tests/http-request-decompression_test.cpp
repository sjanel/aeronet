#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aeronet/compression-config.hpp"
#include "aeronet/decompression-config.hpp"
#include "aeronet/features.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_util.hpp"
#include "simple-charconv.hpp"
#ifdef AERONET_ENABLE_ZLIB
#include "zlib-encoder.hpp"
#endif
#ifdef AERONET_ENABLE_ZSTD
#include "zstd-encoder.hpp"
#endif
#ifdef AERONET_ENABLE_BROTLI
#include "brotli-encoder.hpp"
#endif

using namespace aeronet;

namespace {

[[maybe_unused]] constexpr std::size_t kChunkSize = 256;  // small chunk size to ensure chunk loop is properly tested

std::string gzipCompress([[maybe_unused]] std::string_view input) {
#ifdef AERONET_ENABLE_ZLIB
  CompressionConfig cc;  // defaults; level taken from cfg.zlib.level
  ZlibEncoder encoder(details::ZStreamRAII::Variant::gzip, cc);
  return std::string(encoder.encodeFull(kChunkSize, input));
#else
  return {};
#endif
}

std::string deflateCompress([[maybe_unused]] std::string_view input) {
#ifdef AERONET_ENABLE_ZLIB
  CompressionConfig cc;
  ZlibEncoder encoder(details::ZStreamRAII::Variant::deflate, cc);
  return std::string(encoder.encodeFull(kChunkSize, input));
#else
  return {};
#endif
}

std::string zstdCompress([[maybe_unused]] std::string_view input) {
#ifdef AERONET_ENABLE_ZSTD
  CompressionConfig cc;  // zstd tuning from default config
  ZstdEncoder zencoder(cc);
  return std::string(zencoder.encodeFull(kChunkSize, input));
#else
  return {};
#endif
}

std::string brotliCompress([[maybe_unused]] std::string_view input) {
#ifdef AERONET_ENABLE_BROTLI
  CompressionConfig cc;  // defaults; quality/window from cfg.brotli
  BrotliEncoder encoder(cc);
  return std::string(encoder.encodeFull(kChunkSize, input));
#else
  return {};
#endif
}

struct ClientRawResponse {
  int status{};
  std::string body;
  std::string headersRaw;
};

ClientRawResponse rawPost(uint16_t port, const std::string& target,
                          const std::vector<std::pair<std::string, std::string>>& headers, const std::string& body) {
  test::RequestOptions opt;
  opt.method = http::POST;
  opt.target = target;
  opt.connection = http::close;
  opt.body = body;
  opt.headers = headers;
  auto raw = test::request(port, opt);
  if (!raw) {
    throw std::runtime_error("request failed");
  }
  ClientRawResponse resp;
  auto& str = *raw;
  if (str.empty()) {
    throw std::runtime_error("empty response from server");
  }
  auto firstSpace = str.find(' ');
  if (firstSpace == std::string::npos) {
    throw std::runtime_error(std::string("malformed status line in response: ") + str);
  }
  auto secondSpace = str.find(' ', firstSpace + 1);
  if (secondSpace == std::string::npos) {
    throw std::runtime_error(std::string("malformed status line in response: ") + str);
  }
  resp.status = read3(str.substr(firstSpace + 1, secondSpace - firstSpace - 1).data());
  auto headersEnd = str.find(http::DoubleCRLF);
  if (headersEnd == std::string::npos) {
    throw std::runtime_error(std::string("missing header/body separator (CRLFCRLF) in response: ") + str);
  }
  resp.headersRaw = str.substr(0, headersEnd);
  resp.body = str.substr(headersEnd + http::DoubleCRLF.size());
  return resp;
}

test::TestServer ts(HttpServerConfig{});

}  // namespace

TEST(HttpRequestDecompression, SingleGzip) {
  if constexpr (!zlibEnabled()) {
    GTEST_SKIP();
  }
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.decompression = {}; });
  std::string plain = "HelloCompressedWorld";
  ts.server.router().setDefault([plain](const HttpRequest& req) {
    EXPECT_EQ(req.body(), plain);
    HttpResponse resp;
    resp.body("OK");
    return resp;
  });
  auto comp = gzipCompress(plain);
  auto resp = rawPost(ts.port(), "/g", {{"Content-Encoding", "gzip"}}, comp);
  EXPECT_EQ(resp.status, 200) << resp.headersRaw;
}

TEST(HttpRequestDecompression, SingleDeflate) {
  if constexpr (!zlibEnabled()) {
    GTEST_SKIP();
  }
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.decompression = {}; });
  std::string plain = std::string(10000, 'A');
  ts.server.router().setDefault([plain](const HttpRequest& req) {
    EXPECT_EQ(req.body(), plain);
    HttpResponse resp;
    resp.body("Z");
    return resp;
  });
  auto comp = deflateCompress(plain);
  auto resp = rawPost(ts.port(), "/d", {{"Content-Encoding", "deflate"}}, comp);
  EXPECT_EQ(resp.status, 200);
}

TEST(HttpRequestDecompression, SingleZstd) {
  if constexpr (!zstdEnabled()) {
    GTEST_SKIP();
  }
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.decompression = {}; });
  std::string plain = std::string(10000, 'Z');
  ts.server.router().setDefault([plain](const HttpRequest& req) {
    EXPECT_EQ(req.body(), plain);
    HttpResponse resp;
    resp.body("S");
    return resp;
  });
  auto comp = zstdCompress(plain);
  auto resp = rawPost(ts.port(), "/z", {{"Content-Encoding", "zstd"}}, comp);
  EXPECT_EQ(resp.status, 200);
}

TEST(HttpRequestDecompression, SingleBrotli) {
  if constexpr (!brotliEnabled()) {
    GTEST_SKIP();
  }
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.decompression = {}; });
  std::string plain = std::string(10000, 'B');
  ts.server.router().setDefault([plain](const HttpRequest& req) {
    EXPECT_EQ(req.body(), plain);
    HttpResponse resp;
    resp.body("BR");
    return resp;
  });
  auto comp = brotliCompress(plain);
  auto resp = rawPost(ts.port(), "/br_single", {{"Content-Encoding", "br"}}, comp);
  EXPECT_EQ(resp.status, 200) << resp.headersRaw;
}

TEST(HttpRequestDecompression, MultiGzipDeflateNoSpaces) {
  if constexpr (!zlibEnabled()) {
    GTEST_SKIP();
  }
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.decompression = {}; });
  std::string plain = "MultiStagePayload";
  ts.server.router().setDefault([plain](const HttpRequest& req) {
    EXPECT_EQ(req.body(), plain);
    HttpResponse resp;
    resp.body("M");
    return resp;
  });
  auto deflated = deflateCompress(plain);
  auto gzipped = gzipCompress(deflated);
  auto resp = rawPost(ts.port(), "/m1", {{"Content-Encoding", "deflate,gzip"}}, gzipped);
  EXPECT_EQ(resp.status, 200);
}

TEST(HttpRequestDecompression, MultiZstdGzipWithSpaces) {
  if constexpr (!(zstdEnabled() && zlibEnabled())) {
    GTEST_SKIP();
  }
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.decompression = {}; });
  std::string plain = std::string(10000, 'Q');
  ts.server.router().setDefault([plain](const HttpRequest& req) {
    EXPECT_EQ(req.body(), plain);
    HttpResponse resp;
    resp.body("M2");
    return resp;
  });
  auto gz = gzipCompress(plain);
  auto zstd = zstdCompress(gz);
  auto resp = rawPost(ts.port(), "/m2", {{"Content-Encoding", "gzip, zstd"}}, zstd);
  EXPECT_EQ(resp.status, 200);
}

TEST(HttpRequestDecompression, MultiGzipBrotli) {
  if constexpr (!(zlibEnabled() && brotliEnabled())) {
    GTEST_SKIP();
  }
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.decompression = {}; });
  std::string plain = std::string(10000, 'R');
  ts.server.router().setDefault([plain](const HttpRequest& req) {
    EXPECT_EQ(req.body(), plain);
    HttpResponse resp;
    resp.body("GB");
    return resp;
  });
  auto gz = gzipCompress(plain);  // first stage
  auto br = brotliCompress(gz);   // second (header lists first-applied first)
  auto resp = rawPost(ts.port(), "/gb", {{"Content-Encoding", "gzip, br"}}, br);
  EXPECT_EQ(resp.status, 200);
}

TEST(HttpRequestDecompression, MultiZstdBrotli) {
  if constexpr (!(zstdEnabled() && brotliEnabled())) {
    GTEST_SKIP();
  }
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.decompression = {}; });
  std::string plain = std::string(10000, 'Z');
  ts.server.router().setDefault([plain](const HttpRequest& req) {
    EXPECT_EQ(req.body(), plain);
    HttpResponse resp;
    resp.body("ZB");
    return resp;
  });
  auto zs = zstdCompress(plain);  // first stage (zstd)
  auto br = brotliCompress(zs);   // second
  auto resp = rawPost(ts.port(), "/zb", {{"Content-Encoding", "zstd, br"}}, br);
  EXPECT_EQ(resp.status, 200);
}

TEST(HttpRequestDecompression, IdentitySkippedInChain) {
  if constexpr (!zlibEnabled()) {
    GTEST_SKIP();
  }
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.decompression = {}; });
  std::string plain = "SkipIdentity";
  ts.server.router().setDefault([plain](const HttpRequest& req) {
    EXPECT_EQ(req.body(), plain);
    HttpResponse resp;
    resp.body("I");
    return resp;
  });
  auto deflated = deflateCompress(plain);
  auto gzipped = gzipCompress(deflated);
  auto resp = rawPost(ts.port(), "/i", {{"Content-Encoding", "deflate, identity, gzip"}}, gzipped);
  EXPECT_EQ(resp.status, 200);
}

TEST(HttpRequestDecompression, UnknownCodingRejected) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.decompression = {}; });
  ts.server.router().setDefault([]([[maybe_unused]] const HttpRequest& req) {
    HttpResponse resp;
    resp.body("U");
    return resp;
  });
  std::string body = "abc";  // not used
  auto resp = rawPost(ts.port(), "/u", {{"Content-Encoding", "snappy"}}, body);
  EXPECT_EQ(resp.status, http::StatusCodeUnsupportedMediaType);
}

TEST(HttpRequestDecompression, EmptyTokenRejected) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.decompression = {}; });
  std::string body = "xyz";
  auto resp = rawPost(ts.port(), "/e", {{"Content-Encoding", "identity,,identity"}}, body);
  // empty token forbidden
  EXPECT_EQ(resp.status, 400);
}

TEST(HttpRequestDecompression, DisabledFeaturePassThrough) {
  if constexpr (!zlibEnabled()) {
    GTEST_SKIP();
  }

  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.decompression = {};
    cfg.decompression.enable = false;
  });

  std::string plain = "ABC";
  auto gz = gzipCompress(plain);
  ts.server.router().setDefault([](const HttpRequest& req) {
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
}

TEST(HttpRequestDecompression, ExpansionRatioGuard) {
  if constexpr (!zlibEnabled()) {
    GTEST_SKIP();
  }

  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.decompression = {};
    cfg.decompression.maxExpansionRatio = 2.0;
    cfg.decompression.maxDecompressedBytes = 100000;
  });
  // Highly compressible large input -> gzip then send; expect rejection if ratio >2
  std::string large(100000, 'A');
  auto gz = gzipCompress(large);
  // Ensure it actually compresses well
  ASSERT_LT(gz.size() * 2, large.size());
  auto resp = rawPost(ts.port(), "/rg", {{"Content-Encoding", "gzip"}}, gz);
  EXPECT_EQ(resp.status, 413);
}

// ---------------- Additional whitespace / casing / edge chain tests ----------------

TEST(HttpRequestDecompression, MultiZstdGzipMultiSpaces) {
  if constexpr (!(zstdEnabled() && zlibEnabled())) {
    GTEST_SKIP();
  }
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.decompression = {}; });
  std::string plain = std::string(3200, 'S');
  ts.server.router().setDefault([plain](const HttpRequest& req) {
    EXPECT_EQ(req.body(), plain);
    HttpResponse resp;
    resp.body("OK");
    return resp;
  });
  auto gz = gzipCompress(plain);  // first stage
  auto zstd = zstdCompress(gz);   // second stage (listed last in header)
  auto resp = rawPost(ts.port(), "/mspaces", {{"Content-Encoding", "gzip,   zstd"}}, zstd);
  EXPECT_EQ(resp.status, 200);
}

TEST(HttpRequestDecompression, TripleChainSpacesTabs) {
  if constexpr (!(zstdEnabled() && zlibEnabled())) {
    GTEST_SKIP();
  }
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.decompression = {}; });
  std::string plain = "TripleChain";
  ts.server.router().setDefault([plain](const HttpRequest& req) {
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
}

TEST(HttpRequestDecompression, MixedCaseTokens) {
  if constexpr (!zlibEnabled()) {
    GTEST_SKIP();
  }
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.decompression = {}; });
  std::string plain = "CaseCheck";
  ts.server.router().setDefault([plain](const HttpRequest& req) {
    EXPECT_EQ(req.body(), plain);
    HttpResponse resp;
    resp.body("C");
    return resp;
  });
  auto defl = deflateCompress(plain);  // first (leftmost)
  auto gz = gzipCompress(defl);        // second (rightmost)
  auto resp = rawPost(ts.port(), "/case", {{"Content-Encoding", "deflate, GZip"}}, gz);
  EXPECT_EQ(resp.status, 200);
}

TEST(HttpRequestDecompression, IdentityRepeated) {
  if constexpr (!zlibEnabled()) {
    GTEST_SKIP();
  }
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.decompression = {}; });
  std::string plain = "IdentityRepeat";
  ts.server.router().setDefault([plain](const HttpRequest& req) {
    EXPECT_EQ(req.body(), plain);
    HttpResponse resp;
    resp.body("IR");
    return resp;
  });
  auto defl = deflateCompress(plain);
  auto gz = gzipCompress(defl);
  auto resp = rawPost(ts.port(), "/idrep", {{"Content-Encoding", "deflate, identity, gzip, identity"}}, gz);
  EXPECT_EQ(resp.status, 200);
}

TEST(HttpRequestDecompression, TabsBetweenTokens) {
  if constexpr (!zlibEnabled()) {
    GTEST_SKIP();
  }
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.decompression = {}; });
  std::string plain = "TabsBetween";
  ts.server.router().setDefault([plain](const HttpRequest& req) {
    EXPECT_EQ(req.body(), plain);
    HttpResponse resp;
    resp.body("TB");
    return resp;
  });
  auto defl = deflateCompress(plain);
  auto gz = gzipCompress(defl);
  auto resp = rawPost(ts.port(), "/tabs", {{"Content-Encoding", "deflate,\tgzip"}}, gz);
  EXPECT_EQ(resp.status, 200);
}

TEST(HttpRequestDecompression, UnknownCodingWithSpacesRejected) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.decompression = {}; });
  std::string plain = "UnknownSpace";
  // Apply first encoding so that unknown token appears last.
  auto gz = [&] {
    if constexpr (zlibEnabled()) {
      return gzipCompress(plain);
    }
    return plain;
  }();
  if constexpr (brotliEnabled()) {
    auto resp = rawPost(ts.port(), "/ubr", {{"Content-Encoding", "gzip,  snappy"}}, gz);
    EXPECT_EQ(resp.status, http::StatusCodeUnsupportedMediaType);
  } else {
    auto resp = rawPost(ts.port(), "/ubr", {{"Content-Encoding", "gzip,  br"}}, gz);
    EXPECT_EQ(resp.status, http::StatusCodeUnsupportedMediaType);
  }
}

TEST(HttpRequestDecompression, EmptyTokenWithSpacesRejected) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.decompression = {}; });
  std::string body = "abc123";  // intentionally not compressed
  auto resp = rawPost(ts.port(), "/emptsp", {{"Content-Encoding", "identity,  ,identity"}}, body);
  EXPECT_EQ(resp.status, 400);
}

// ---------------- Corruption / truncated frame tests ----------------

TEST(HttpRequestDecompression, CorruptedGzipTruncatedTail) {
  if constexpr (!zlibEnabled()) {
    GTEST_SKIP();
  }
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.decompression = {}; });
  std::string plain = std::string(200, 'G');
  ts.server.router().setDefault([plain](const HttpRequest& req) {
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
}

TEST(HttpRequestDecompression, CorruptedZstdBadMagic) {
  if constexpr (!zstdEnabled()) {
    GTEST_SKIP();
  }
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.decompression = {}; });
  std::string plain = std::string(512, 'Z');
  ts.server.router().setDefault([plain](const HttpRequest& req) {
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
}

TEST(HttpRequestDecompression, CorruptedBrotliTruncated) {
  if constexpr (!brotliEnabled()) {
    GTEST_SKIP();
  }
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.decompression = {}; });
  std::string plain = std::string(300, 'B');
  ts.server.router().setDefault([plain](const HttpRequest& req) {
    EXPECT_EQ(req.body(), plain);  // Not reached for corrupted case
    HttpResponse resp;
    resp.body("OK");
    return resp;
  });
  auto full = brotliCompress(plain);
  ASSERT_GT(full.size(), 8U);
  auto truncated = full.substr(0, full.size() - 4);
  auto resp = rawPost(ts.port(), "/cbr", {{"Content-Encoding", "br"}}, truncated);
  EXPECT_EQ(resp.status, 400) << "Expected 400 for truncated brotli stream";
}
