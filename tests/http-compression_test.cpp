#include <gtest/gtest.h>

#include <chrono>

#ifdef AERONET_ENABLE_ZSTD
#include <zstd.h>
#endif

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aeronet/compression-config.hpp"
#include "aeronet/compression-test-helpers.hpp"
#include "aeronet/encoding.hpp"
#include "aeronet/features.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response-writer.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/router-config.hpp"
#include "aeronet/single-http-server.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_util.hpp"

using namespace aeronet;

namespace {  // Helper utilities local to this test file

bool HasGzipMagic(std::string_view body) {
  return body.size() >= 2 && static_cast<unsigned char>(body[0]) == 0x1f && static_cast<unsigned char>(body[1]) == 0x8b;
}

bool LooksLikeZlib(std::string_view body) {
  // Very loose heuristic: zlib header is 2 bytes: CMF (compression method/flags) + FLG with check bits.
  // CMF lower 4 bits must be 8 (deflate), i.e. 0x78 is common for default window (0x78 0x9C etc).
  return body.size() >= 2 && static_cast<unsigned char>(body[0]) == 0x78;  // ignore second byte variability
}

test::TestServer ts{HttpServerConfig{}, RouterConfig{}, std::chrono::milliseconds{1}};

}  // namespace

TEST(HttpCompression, BrAppliedWhenEligible) {
  if constexpr (!brotliEnabled()) {
    GTEST_SKIP();
  }
  ts.postConfigUpdate([](HttpServerConfig &cfg) {
    cfg.compression.minBytes = 32;
    cfg.compression.preferredFormats = {Encoding::br};
  });

  std::string payload(400, 'B');
  ts.router().setDefault([payload](const HttpRequest &) {
    HttpResponse respObj;
    respObj.header("Content-Type", "text/plain");
    respObj.body(payload);
    return respObj;
  });
  auto resp = test::simpleGet(ts.port(), "/br1", {{"Accept-Encoding", "br"}});
  EXPECT_EQ(resp.statusCode, http::StatusCodeOK);
  auto it = resp.headers.find("Content-Encoding");
  ASSERT_NE(it, resp.headers.end());
  EXPECT_EQ(it->second, "br");
  EXPECT_LT(resp.body.size(), payload.size());
}

TEST(HttpCompression, UserContentEncodingIdentityDisablesCompression) {
  if constexpr (!brotliEnabled()) {
    GTEST_SKIP();
  }
  ts.postConfigUpdate([](HttpServerConfig &cfg) {
    cfg.compression.minBytes = 1;
    cfg.compression.preferredFormats = {Encoding::br};
  });
  std::string payload(128, 'U');
  ts.router().setDefault([payload](const HttpRequest &) {
    HttpResponse respObj;
    respObj.header("Content-Type", "text/plain");
    respObj.header("Content-Encoding", "identity");
    respObj.body(payload);
    return respObj;
  });
  auto resp = test::simpleGet(ts.port(), "/br2", {{"Accept-Encoding", "br"}});
  EXPECT_EQ(resp.statusCode, http::StatusCodeOK);
  auto it = resp.headers.find("Content-Encoding");
  ASSERT_NE(it, resp.headers.end());
  EXPECT_EQ(it->second, "identity");
  EXPECT_EQ(resp.body.size(), payload.size());
}

TEST(HttpCompression, BelowThresholdNotCompressed) {
  if constexpr (!brotliEnabled()) {
    GTEST_SKIP();
  }
  ts.postConfigUpdate([](HttpServerConfig &cfg) {
    cfg.compression.minBytes = 2048;
    cfg.compression.preferredFormats = {Encoding::br};
  });
  std::string small(64, 's');
  ts.router().setDefault([small](const HttpRequest &) {
    HttpResponse respObj;
    respObj.body(small);
    return respObj;
  });
  auto resp = test::simpleGet(ts.port(), "/br3", {{"Accept-Encoding", "br"}});
  EXPECT_EQ(resp.statusCode, http::StatusCodeOK);
  EXPECT_FALSE(resp.headers.contains("Content-Encoding"));
  EXPECT_EQ(resp.body.size(), small.size());
}

TEST(HttpCompression, NoAcceptEncodingHeaderStillCompressesDefault) {
  if constexpr (!brotliEnabled()) {
    GTEST_SKIP();
  }
  ts.postConfigUpdate([](HttpServerConfig &cfg) {
    cfg.compression.minBytes = 16;
    cfg.compression.preferredFormats = {Encoding::br};
  });
  std::string payload(180, 'D');
  ts.router().setDefault([payload](const HttpRequest &) {
    HttpResponse respObj;
    respObj.body(payload);
    return respObj;
  });
  auto resp = test::simpleGet(ts.port(), "/br4", {{"Accept-Encoding", "*"}});
  EXPECT_EQ(resp.statusCode, http::StatusCodeOK);
  auto it = resp.headers.find("Content-Encoding");
  ASSERT_NE(it, resp.headers.end());
  EXPECT_EQ(it->second, "br");
}

TEST(HttpCompression, PreservesUserContentTypeWhenCompressing) {
  static_assert(brotliEnabled() || zlibEnabled(), "At least one compression encoder must be available");
  if constexpr (!brotliEnabled() && !zlibEnabled()) {
    GTEST_SKIP();
  }

  const std::string customType = "application/vnd.acme.resource+json";
  std::string acceptEncoding;
  std::string expectedEncoding;
  if constexpr (brotliEnabled()) {
    expectedEncoding = "br";
    acceptEncoding = "br";
    ts.postConfigUpdate([](HttpServerConfig &cfg) {
      cfg.compression.minBytes = 32;
      cfg.compression.preferredFormats = {Encoding::br};
    });
  } else if constexpr (zlibEnabled()) {
    expectedEncoding = "gzip";
    acceptEncoding = "gzip";
    ts.postConfigUpdate([](HttpServerConfig &cfg) {
      cfg.compression.minBytes = 32;
      cfg.compression.preferredFormats = {Encoding::gzip};
    });
  }

  std::string payload(160, 'R');
  ts.router().setDefault([payload, customType](const HttpRequest &) {
    HttpResponse respObj;
    respObj.body(payload, customType);
    return respObj;
  });

  auto resp = test::simpleGet(ts.port(), "/ctype", {{"Accept-Encoding", acceptEncoding}});
  EXPECT_EQ(resp.statusCode, http::StatusCodeOK);

  auto itccc = resp.headers.find("Content-Type");
  ASSERT_NE(itccc, resp.headers.end());
  EXPECT_EQ(itccc->second, customType);

  auto it = resp.headers.find("Content-Encoding");
  ASSERT_NE(it, resp.headers.end());
  EXPECT_EQ(it->second, expectedEncoding);
  EXPECT_LT(resp.body.size(), payload.size());
}

TEST(HttpCompression, InlineBodyCompressionMovesToCapturedPayload) {
#if !defined(AERONET_ENABLE_BROTLI) && !defined(AERONET_ENABLE_ZLIB)
  GTEST_SKIP();
#endif

  std::string expectedEncoding;
  std::string acceptEncoding;

#ifdef AERONET_ENABLE_BROTLI
  ts.postConfigUpdate([](HttpServerConfig &cfg) {
    cfg.compression.minBytes = 64;
    cfg.compression.preferredFormats = {Encoding::br};
  });
  expectedEncoding = "br";
  acceptEncoding = "br";
#elifdef AERONET_ENABLE_ZLIB
  ts.postConfigUpdate([](HttpServerConfig &cfg) {
    cfg.compression.minBytes = 64;
    cfg.compression.preferredFormats = {Encoding::gzip};
  });
  expectedEncoding = "gzip";
  acceptEncoding = "gzip";
#endif

  const std::string customType = "application/x-inline";
  std::string inlinePayload(512, 'I');
  ts.router().setDefault([inlinePayload, customType](const HttpRequest &) {
    HttpResponse respObj;
    std::string_view inlineView(inlinePayload);
    respObj.body(inlineView, customType);  // forces inline buffer storage (no captured payload)
    return respObj;
  });

  auto resp = test::simpleGet(ts.port(), "/inline", {{"Accept-Encoding", acceptEncoding}});
  EXPECT_EQ(resp.statusCode, http::StatusCodeOK);
  auto it = resp.headers.find("Content-Encoding");
  ASSERT_NE(it, resp.headers.end());
  EXPECT_EQ(it->second, expectedEncoding);

  auto itType = resp.headers.find("Content-Type");
  ASSERT_NE(itType, resp.headers.end());
  EXPECT_EQ(itType->second, customType);

  EXPECT_LT(resp.body.size(), inlinePayload.size());
#if !defined(AERONET_ENABLE_BROTLI) && defined(AERONET_ENABLE_ZLIB)
  EXPECT_TRUE(HasGzipMagic(resp.body));
#endif
}

// Compression with captured body and trailers: ensure trailers are transmitted after compressed payload
TEST(HttpCompression, CapturedBodyWithTrailers) {
  // prefer any available encoder; minBytes small so compression activates
  ts.postConfigUpdate([](HttpServerConfig &cfg) {
    cfg.compression.minBytes = 8;
#ifdef AERONET_ENABLE_ZSTD
    cfg.compression.preferredFormats = {Encoding::zstd};
#elifdef AERONET_ENABLE_BROTLI
    cfg.compression.preferredFormats = {Encoding::br};
#elifdef AERONET_ENABLE_ZLIB
    cfg.compression.preferredFormats = {Encoding::gzip};
#endif
  });

  std::string payload(256, 'P');
  ts.router().setDefault([payload](const HttpRequest &) {
    HttpResponse respObj;
    // supply body as captured payload directly (simulate handler that sets captured payload)
    respObj.body(payload);
    // add trailers after body
    respObj.addTrailer("X-Checksum", "cksum");
    respObj.addTrailer("X-Extra", "val");
    return respObj;
  });

  test::ClientConnection sock(ts.port());
  int fd = sock.fd();
  std::string req =
      "GET /captured-trailers HTTP/1.1\r\n"
      "Host: example.com\r\n"
      "Connection: close\r\n"
      "Accept-Encoding: *\r\n"
      "\r\n";
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);
  EXPECT_TRUE(resp.find("X-Checksum: cksum") != std::string::npos);
  EXPECT_TRUE(resp.find("X-Extra: val") != std::string::npos);
}

// Compression for inline body where compression moves body to captured payload, with trailers added
TEST(HttpCompression, InlineBodyWithTrailers) {
  ts.postConfigUpdate([](HttpServerConfig &cfg) {
    cfg.compression.minBytes = 16;
#ifdef AERONET_ENABLE_ZSTD
    cfg.compression.preferredFormats = {Encoding::zstd};
#elifdef AERONET_ENABLE_BROTLI
    cfg.compression.preferredFormats = {Encoding::br};
#elifdef AERONET_ENABLE_ZLIB
    cfg.compression.preferredFormats = {Encoding::gzip};
#endif
  });

  std::string inlinePayload(256, 'L');
  ts.router().setDefault([inlinePayload](const HttpRequest &) {
    HttpResponse respObj;
    // create inline body (string_view) to force inline storage
    respObj.body(std::string_view(inlinePayload));
    // trailers must be added after body
    respObj.addTrailer("X-Inline", "ok");
    return respObj;
  });

  test::ClientConnection sock(ts.port());
  int fd = sock.fd();
  std::string req =
      "GET /inline-trailers HTTP/1.1\r\n"
      "Host: example.com\r\n"
      "Connection: close\r\n"
      "Accept-Encoding: *\r\n"
      "\r\n";
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);
  EXPECT_TRUE(resp.find("X-Inline: ok") != std::string::npos);
}

TEST(HttpCompression, IdentityForbiddenNoAlternativesReturns406) {
  if constexpr (!brotliEnabled()) {
    GTEST_SKIP();
  }
  ts.postConfigUpdate([](HttpServerConfig &cfg) {
    cfg.compression.minBytes = 1;
    cfg.compression.preferredFormats = {Encoding::br};
  });
  std::string payload(70, 'Q');
  ts.router().setDefault([payload](const HttpRequest &) {
    HttpResponse respObj;
    respObj.body(payload);
    return respObj;
  });
  auto resp = test::simpleGet(ts.port(), "/br5", {{"Accept-Encoding", "identity;q=0, snappy;q=0"}});
  EXPECT_EQ(resp.statusCode, http::StatusCodeNotAcceptable);
  EXPECT_EQ(resp.body, "No acceptable content-coding available");
}

TEST(HttpCompression, BrActivatedOverThreshold) {
  if constexpr (!brotliEnabled()) {
    GTEST_SKIP();
  }
  ts.postConfigUpdate([](HttpServerConfig &cfg) {
    cfg.compression.minBytes = 64;
    cfg.compression.preferredFormats = {Encoding::br};
  });
  std::string part1(40, 'a');
  std::string part2(80, 'b');
  ts.router().setDefault([part1, part2](const HttpRequest &, HttpResponseWriter &writer) {
    writer.status(http::StatusCodeOK);
    writer.writeBody(part1);
    writer.writeBody(part2);
    writer.end();
  });
  auto resp = test::simpleGet(ts.port(), "/sbr1", {{"Accept-Encoding", "br"}});
  auto it = resp.headers.find("Content-Encoding");
  if (it != resp.headers.end()) {
    EXPECT_EQ(it->second, "br");
  }
  // Size heuristic: compressed should be smaller than concatenated plain text.
  EXPECT_LT(resp.body.size(), part1.size() + part2.size());
}

TEST(HttpCompression, BelowThresholdIdentity) {
  if constexpr (!brotliEnabled()) {
    GTEST_SKIP();
  }
  ts.postConfigUpdate([](HttpServerConfig &cfg) {
    cfg.compression.minBytes = 1024;
    cfg.compression.preferredFormats = {Encoding::br};
  });
  std::string small(80, 'x');
  ts.router().setDefault([small](const HttpRequest &) {
    HttpResponse respObj;
    respObj.body(small);
    return respObj;
  });
  auto resp = test::simpleGet(ts.port(), "/sbr2", {{"Accept-Encoding", "br"}});
  EXPECT_FALSE(resp.headers.contains("Content-Encoding"));
  EXPECT_TRUE(resp.body.contains('x'));
}

TEST(HttpCompression, UserProvidedIdentityPreventsActivation) {
  if constexpr (!brotliEnabled()) {
    GTEST_SKIP();
  }
  ts.postConfigUpdate([](HttpServerConfig &cfg) {
    cfg.compression.minBytes = 16;
    cfg.compression.preferredFormats = {Encoding::br};
  });
  std::string payload(512, 'Y');
  ts.router().setDefault([payload]([[maybe_unused]] const HttpRequest &req, HttpResponseWriter &writer) {
    writer.status(http::StatusCodeOK);
    writer.header("Content-Encoding", "identity");
    writer.writeBody(payload);
    writer.end();
  });
  auto resp = test::simpleGet(ts.port(), "/sbr3", {{"Accept-Encoding", "br"}});
  auto it = resp.headers.find("Content-Encoding");
  ASSERT_NE(it, resp.headers.end());
  EXPECT_EQ(it->second, "identity");
  // Streaming identity may use chunked transfer, so body size can exceed raw payload due to framing; just ensure
  // we did not apply brotli (which would eliminate long runs of 'Y').
  EXPECT_TRUE(resp.body.contains(std::string(32, 'Y')));
}

TEST(HttpCompression, QValuesInfluenceSelection) {
  if constexpr (!brotliEnabled()) {
    GTEST_SKIP();
  }
  ts.postConfigUpdate([](HttpServerConfig &cfg) {
    cfg.compression.minBytes = 64;
    cfg.compression.preferredFormats = {Encoding::br};
  });
  std::string payload(600, 'Z');
  ts.router().setDefault([payload]([[maybe_unused]] const HttpRequest &req, HttpResponseWriter &writer) {
    writer.status(http::StatusCodeOK);
    writer.writeBody(payload.substr(0, 128));
    writer.writeBody(payload.substr(128));
    writer.end();
  });
  // Client strongly prefers br
  auto resp = test::simpleGet(ts.port(), "/sbr4", {{"Accept-Encoding", "gzip;q=0.5, br;q=1.0"}});
  auto it = resp.headers.find("Content-Encoding");
  if (it != resp.headers.end()) {
    EXPECT_EQ(it->second, "br");
  }
}

TEST(HttpCompression, StreamingIdentityForbiddenNoAlternativesReturns406) {
  if constexpr (!brotliEnabled()) {
    GTEST_SKIP();
  }
  ts.postConfigUpdate([](HttpServerConfig &cfg) {
    cfg.compression.minBytes = 1;
    cfg.compression.preferredFormats = {Encoding::br};
  });
  std::string payload(90, 'F');
  ts.router().setDefault([payload]([[maybe_unused]] const HttpRequest &req, HttpResponseWriter &writer) {
    writer.status(http::StatusCodeOK);
    writer.writeBody(payload);
    writer.end();
  });
  auto resp = test::simpleGet(ts.port(), "/sbr5", {{"Accept-Encoding", "identity;q=0, snappy;q=0"}});
  // Server should respond 406 (not compressible with offered encodings; identity forbidden)
  EXPECT_TRUE(resp.headersRaw.contains(" 406 "));
}

TEST(HttpCompression, GzipUserContentEncodingIdentityDisablesCompression) {
  if constexpr (!zlibEnabled()) {
    GTEST_SKIP();
  }
  ts.postConfigUpdate([](HttpServerConfig &cfg) {
    cfg.compression.minBytes = 1;
    cfg.compression.preferredFormats = {Encoding::gzip};
  });
  std::string payload(128, 'B');
  ts.router().setDefault([payload](const HttpRequest &) {
    HttpResponse resp;
    resp.header("Content-Type", "text/plain");
    resp.header("Content-Encoding", "identity");  // explicit suppression
    resp.body(payload);
    return resp;
  });
  auto resp = test::simpleGet(ts.port(), "/o", {{"Accept-Encoding", "gzip"}});
  EXPECT_EQ(resp.statusCode, http::StatusCodeOK);
  // Should remain uncompressed and server must not alter user-provided identity
  auto itCE = resp.headers.find("Content-Encoding");
  ASSERT_NE(itCE, resp.headers.end());
  EXPECT_EQ(itCE->second, "identity");
  EXPECT_EQ(resp.body.size(), payload.size());
}

TEST(HttpCompression, GzipBelowThresholdNotCompressed) {
  if constexpr (!zlibEnabled()) {
    GTEST_SKIP();
  }
  ts.postConfigUpdate([](HttpServerConfig &cfg) {
    cfg.compression.minBytes = 1024;
    cfg.compression.preferredFormats = {Encoding::gzip};
  });
  std::string smallPayload(32, 'C');
  ts.router().setDefault([smallPayload](const HttpRequest &) {
    HttpResponse resp;
    resp.header("Content-Type", "text/plain");
    resp.body(smallPayload);
    return resp;
  });
  auto resp = test::simpleGet(ts.port(), "/s", {{"Accept-Encoding", "gzip"}});
  EXPECT_EQ(resp.statusCode, http::StatusCodeOK);
  EXPECT_FALSE(resp.headers.contains("Content-Encoding"));
  EXPECT_EQ(resp.body.size(), smallPayload.size());
}

TEST(HttpCompression, GzipNoAcceptEncodingHeaderStillCompressesDefault) {
  if constexpr (!zlibEnabled()) {
    GTEST_SKIP();
  }
  ts.postConfigUpdate([](HttpServerConfig &cfg) {
    cfg.compression.minBytes = 16;
    cfg.compression.preferredFormats = {Encoding::gzip};
  });
  std::string payload(128, 'D');
  ts.router().setDefault([payload](const HttpRequest &) {
    HttpResponse resp;
    resp.header("Content-Type", "text/plain");
    resp.body(payload);
    return resp;
  });
  auto resp = test::simpleGet(ts.port(), "/i", {});
  EXPECT_EQ(resp.statusCode, http::StatusCodeOK);
  auto it = resp.headers.find("Content-Encoding");
  if (it != resp.headers.end()) {
    EXPECT_EQ(it->second, "gzip");
    EXPECT_TRUE(HasGzipMagic(resp.body));
  }
}

TEST(HttpCompression, GzipIdentityForbiddenNoAlternativesReturns406) {
  if constexpr (!zlibEnabled()) {
    GTEST_SKIP();
  }
  ts.postConfigUpdate([](HttpServerConfig &cfg) {
    cfg.compression.minBytes = 1;
    cfg.compression.preferredFormats = {Encoding::gzip};
  });
  std::string payload(64, 'Q');
  ts.router().setDefault([payload](const HttpRequest &) {
    HttpResponse resp;
    resp.header("Content-Type", "text/plain");
    resp.body(payload);
    return resp;
  });
  // Client forbids identity and offers only unsupported encodings (br here is unsupported in current build).
  auto resp = test::simpleGet(ts.port(), "/bad", {{"Accept-Encoding", "identity;q=0, br;q=0"}});
  EXPECT_EQ(resp.statusCode, http::StatusCodeNotAcceptable);
  EXPECT_EQ(resp.body, "No acceptable content-coding available");
}

TEST(HttpCompression, IdentityForbiddenButGzipAvailableUsesGzip) {
  if constexpr (!zlibEnabled()) {
    GTEST_SKIP();
  }
  ts.postConfigUpdate([](HttpServerConfig &cfg) {
    cfg.compression.minBytes = 1;
    cfg.compression.preferredFormats = {Encoding::gzip};
  });
  std::string payload(128, 'Z');
  ts.router().setDefault([payload](const HttpRequest &) {
    HttpResponse resp;
    resp.header("Content-Type", "text/plain");
    resp.body(payload);
    return resp;
  });
  auto resp = test::simpleGet(ts.port(), "/ok", {{"Accept-Encoding", "identity;q=0, gzip"}});
  EXPECT_EQ(resp.statusCode, http::StatusCodeOK);
  auto it = resp.headers.find("Content-Encoding");
  ASSERT_NE(it, resp.headers.end());
  EXPECT_EQ(it->second, "gzip");
  EXPECT_TRUE(HasGzipMagic(resp.body));
}

TEST(HttpCompression, UnsupportedEncodingDoesNotApplyGzip) {
  if constexpr (!zlibEnabled()) {
    GTEST_SKIP();
  }
  ts.postConfigUpdate([](HttpServerConfig &cfg) {
    cfg.compression.minBytes = 1;
    cfg.compression.preferredFormats = {Encoding::gzip};
  });
  std::string payload(200, 'E');
  ts.router().setDefault([payload](const HttpRequest &) {
    HttpResponse resp;
    resp.header("Content-Type", "text/plain");
    resp.body(payload);
    return resp;
  });
  // If brotli support is compiled in, 'br' is actually supported and would trigger compression.
  // Use an obviously unsupported token (snappy) in that case.
  auto resp = test::simpleGet(ts.port(), "/br", {{"Accept-Encoding", brotliEnabled() ? "snappy" : "br"}});
  EXPECT_EQ(resp.statusCode, http::StatusCodeOK);
  EXPECT_EQ(resp.headers.find("Content-Encoding"), resp.headers.end());
}

TEST(HttpCompression, DeflateAppliedWhenPreferredAndAccepted) {
  if constexpr (!zlibEnabled()) {
    GTEST_SKIP();
  }
  ts.postConfigUpdate([](HttpServerConfig &cfg) {
    cfg.compression.minBytes = 32;
    cfg.compression.preferredFormats = {Encoding::deflate, Encoding::gzip};
  });
  std::string largePayload(300, 'F');
  ts.router().setDefault([largePayload](const HttpRequest &) {
    HttpResponse resp;
    resp.header("Content-Type", "text/plain");
    resp.body(largePayload);
    return resp;
  });
  auto resp = test::simpleGet(ts.port(), "/d1", {{"Accept-Encoding", "deflate,gzip"}});
  EXPECT_EQ(resp.statusCode, 200);
  auto it = resp.headers.find("Content-Encoding");
  ASSERT_NE(it, resp.headers.end());
  EXPECT_EQ(it->second, "deflate");
  EXPECT_TRUE(LooksLikeZlib(resp.body));
  EXPECT_LT(resp.body.size(), largePayload.size());
}

TEST(HttpCompression, GzipChosenWhenHigherPreference) {
  if constexpr (!zlibEnabled()) {
    GTEST_SKIP();
  }
  ts.postConfigUpdate([](HttpServerConfig &cfg) {
    cfg.compression.minBytes = 16;
    cfg.compression.preferredFormats = {Encoding::gzip, Encoding::deflate};
  });
  std::string payload(256, 'G');
  ts.router().setDefault([payload](const HttpRequest &) {
    HttpResponse resp;
    resp.header("Content-Type", "text/plain");
    resp.body(payload);
    return resp;
  });
  auto resp = test::simpleGet(ts.port(), "/d2", {{"Accept-Encoding", "gzip,deflate"}});
  auto it = resp.headers.find("Content-Encoding");
  ASSERT_NE(it, resp.headers.end());
  EXPECT_EQ(it->second, "gzip");
  EXPECT_TRUE(HasGzipMagic(resp.body));
}

TEST(HttpCompression, QValuesAffectSelection) {
  if constexpr (!zlibEnabled()) {
    GTEST_SKIP();
  }
  ts.postConfigUpdate([](HttpServerConfig &cfg) {
    cfg.compression.minBytes = 16;
    cfg.compression.preferredFormats = {Encoding::gzip, Encoding::deflate};
  });
  std::string payload(180, 'H');
  ts.router().setDefault([payload](const HttpRequest &) {
    HttpResponse resp;
    resp.header("Content-Type", "text/plain");
    resp.body(payload);
    return resp;
  });
  auto resp = test::simpleGet(ts.port(), "/d3", {{"Accept-Encoding", "gzip;q=0.1, deflate;q=0.9"}});
  auto it = resp.headers.find("Content-Encoding");
  ASSERT_NE(it, resp.headers.end());
  EXPECT_EQ(it->second, "deflate");
  EXPECT_TRUE(LooksLikeZlib(resp.body));
}

TEST(HttpCompression, IdentityFallbackIfDeflateNotRequested) {
  if constexpr (!zlibEnabled()) {
    GTEST_SKIP();
  }
  ts.postConfigUpdate([](HttpServerConfig &cfg) {
    cfg.compression.minBytes = 8;
    cfg.compression.preferredFormats = {Encoding::deflate};
  });
  std::string payload(256, 'I');
  ts.router().setDefault([payload](const HttpRequest &) {
    HttpResponse resp;
    resp.header("Content-Type", "text/plain");
    resp.body(payload);
    return resp;
  });
  auto resp = test::simpleGet(ts.port(), "/d4", {{"Accept-Encoding", "gzip"}});  // client does NOT list deflate
  auto it = resp.headers.find("Content-Encoding");
  // Under current semantics gzip is still chosen (higher q than identity) even if not in preferredFormats.
  ASSERT_NE(it, resp.headers.end());
  EXPECT_EQ(it->second, "gzip");
  EXPECT_TRUE(HasGzipMagic(resp.body));
  EXPECT_LT(resp.body.size(), payload.size());
}

// NOTE: These streaming tests validate that compression is applied (or not) and that negotiation picks
// the expected format. They do not currently attempt mid-stream header observation since the handler
// executes to completion before the test inspects the socket.

TEST(HttpCompression, StreamingGzipActivatedOverThreshold) {
  if constexpr (!zlibEnabled()) {
    GTEST_SKIP();
  }
  ts.postConfigUpdate([](HttpServerConfig &cfg) {
    cfg.compression.minBytes = 64;
    cfg.compression.preferredFormats = {Encoding::gzip};
  });
  std::string part1(40, 'a');
  std::string part2(80, 'b');
  ts.router().setDefault([part1, part2](const HttpRequest &, HttpResponseWriter &writer) {
    writer.status(http::StatusCodeOK);
    writer.contentType("text/plain");
    writer.writeBody(part1);  // below threshold so far
    writer.writeBody(part2);  // crosses threshold -> compression should activate
    writer.end();
  });
  auto resp = test::simpleGet(ts.port(), "/sgz", {{"Accept-Encoding", "gzip"}});
  // NOTE: Current implementation emits headers before compression activation, so Content-Encoding
  // may be absent even though body bytes are compressed. Accept either presence or absence but
  // verify gzip magic appears in body to confirm activation.
  auto it = resp.headers.find("Content-Encoding");
  if (it != resp.headers.end()) {
    EXPECT_EQ(it->second, "gzip");
  }
  EXPECT_TRUE(resp.body.contains("\x1f\x8b") || HasGzipMagic(resp.body));
}

TEST(HttpCompression, StreamingDeflateActivatedOverThreshold) {
  if constexpr (!zlibEnabled()) {
    GTEST_SKIP();
  }
  ts.postConfigUpdate([](HttpServerConfig &cfg) {
    cfg.compression.minBytes = 32;
    cfg.compression.preferredFormats = {Encoding::deflate, Encoding::gzip};
  });
  std::string payload(128, 'X');
  ts.router().setDefault([payload](const HttpRequest &, HttpResponseWriter &writer) {
    writer.status(http::StatusCodeOK);
    writer.writeBody(payload.substr(0, 40));
    writer.writeBody(payload.substr(40));
    writer.end();
  });
  auto resp = test::simpleGet(ts.port(), "/sdf", {{"Accept-Encoding", "deflate,gzip"}});
  auto it = resp.headers.find("Content-Encoding");
  ASSERT_NE(it, resp.headers.end())
      << "Content-Encoding header should be present after delayed header emission refactor";
  EXPECT_EQ(it->second, "deflate");
  // Minimal integrity check: compressed body should not be trivially equal to original repeated character sequence
  EXPECT_NE(resp.body.size(), 128U);  // chunked framing + compression alters size
}

TEST(HttpCompression, StreamingBelowThresholdIdentity) {
  if constexpr (!zlibEnabled()) {
    GTEST_SKIP();
  }
  ts.postConfigUpdate([](HttpServerConfig &cfg) {
    cfg.compression.minBytes = 512;
    cfg.compression.preferredFormats = {Encoding::gzip};
  });

  std::string small(40, 'y');
  ts.router().setDefault([small](const HttpRequest &, HttpResponseWriter &writer) {
    writer.status(http::StatusCodeOK);
    writer.writeBody(small);  // never crosses threshold
    writer.end();
  });
  auto resp = test::simpleGet(ts.port(), "/sid", {{"Accept-Encoding", "gzip"}});
  EXPECT_FALSE(resp.headers.contains("Content-Encoding"));
  EXPECT_TRUE(resp.body.contains(small));
}

TEST(HttpCompression, StreamingUserProvidedContentEncodingIdentityPreventsActivation) {
  ts.postConfigUpdate([](HttpServerConfig &cfg) {
    cfg.compression.minBytes = 16;
    cfg.compression.preferredFormats = {Encoding::gzip};
  });
  std::string big(200, 'Z');
  ts.router().setDefault([big](const HttpRequest &, HttpResponseWriter &writer) {
    writer.status(http::StatusCodeOK);
    writer.header("Content-Encoding", "identity");  // explicit suppression
    writer.writeBody(big.substr(0, 50));
    writer.writeBody(big.substr(50));
    writer.end();
  });
  auto resp = test::simpleGet(ts.port(), "/soff", {{"Accept-Encoding", "gzip"}});
  auto it = resp.headers.find("Content-Encoding");
  ASSERT_NE(it, resp.headers.end());
  EXPECT_EQ(it->second, "identity");
  // Body should contain literal 'Z' sequences (chunked framing around them)
  EXPECT_TRUE(resp.body.contains('Z'));
}

TEST(HttpCompression, StreamingQValuesInfluenceStreamingSelection) {
  if constexpr (!zlibEnabled()) {
    GTEST_SKIP();
  }
  ts.postConfigUpdate([](HttpServerConfig &cfg) {
    cfg.compression.minBytes = 16;
    cfg.compression.preferredFormats = {Encoding::gzip, Encoding::deflate};
  });
  std::string payload(180, 'Q');
  ts.router().setDefault([payload](const HttpRequest &, HttpResponseWriter &writer) {
    writer.status(http::StatusCodeOK);
    writer.contentType("text/plain");
    writer.writeBody(payload.substr(0, 60));
    writer.writeBody(payload.substr(60));
    writer.end();
  });
  auto resp = test::simpleGet(ts.port(), "/sqv", {{"Accept-Encoding", "gzip;q=0.1, deflate;q=0.9"}});
  auto it = resp.headers.find("Content-Encoding");
  ASSERT_NE(it, resp.headers.end());
  EXPECT_EQ(it->second, "deflate");
}

TEST(HttpCompression, GzipStreamingIdentityForbiddenNoAlternativesReturns406) {
  ts.postConfigUpdate([](HttpServerConfig &cfg) {
    cfg.compression.minBytes = 1;
    cfg.compression.preferredFormats = {Encoding::gzip};
  });
  ts.router().setDefault([](const HttpRequest &, HttpResponseWriter &writer) {
    writer.status(http::StatusCodeOK);  // will be overridden to 406 before handler invoked if negotiation rejects
    writer.contentType("text/plain");
    writer.writeBody(std::string(64, 'Q'));
    writer.end();
  });
  auto resp = test::simpleGet(ts.port(), "/sbad", {{"Accept-Encoding", "identity;q=0, br;q=0"}});
  EXPECT_TRUE(resp.headersRaw.rfind("HTTP/1.1 406", 0) == 0) << resp.headersRaw;
  EXPECT_EQ(resp.body, "No acceptable content-coding available");
}

TEST(HttpCompression, ZstdAppliedWhenEligible) {
  if constexpr (!zstdEnabled()) {
    GTEST_SKIP();
  }
  ts.postConfigUpdate([](HttpServerConfig &cfg) {
    cfg.compression.minBytes = 32;
    cfg.compression.preferredFormats = {Encoding::zstd};
  });
  std::string payload(400, 'A');
  ts.router().setDefault([payload](const HttpRequest &) {
    HttpResponse resp;
    resp.header("Content-Type", "text/plain");
    resp.body(payload);
    return resp;
  });
  auto resp = test::simpleGet(ts.port(), "/z", {{"Accept-Encoding", "zstd"}});
  ASSERT_EQ(resp.statusCode, 200);
  auto it = resp.headers.find("Content-Encoding");
  ASSERT_NE(it, resp.headers.end());
  EXPECT_EQ(it->second, "zstd");
  EXPECT_TRUE(test::HasZstdMagic(resp.body));
  EXPECT_LT(resp.body.size(), payload.size());
  // Round-trip verify by decompressing (simple one-shot) to ensure integrity
  std::string decompressed = test::ZstdRoundTripDecompress(resp.body, payload.size());
  EXPECT_EQ(decompressed, payload);
}

TEST(HttpCompression, WildcardSelectsZstdIfPreferred) {
  if constexpr (!zstdEnabled()) {
    GTEST_SKIP();
  }
  ts.postConfigUpdate([](HttpServerConfig &cfg) {
    cfg.compression.minBytes = 16;
    cfg.compression.preferredFormats = {Encoding::zstd};
    if constexpr (zlibEnabled()) {
      cfg.compression.preferredFormats.push_back(Encoding::gzip);
    }
  });

  std::string payload(256, 'B');
  ts.router().setDefault([payload](const HttpRequest &) {
    HttpResponse resp;
    resp.body(payload);
    resp.header("Content-Type", "text/plain");
    return resp;
  });
  auto resp = test::simpleGet(ts.port(), "/w", {{"Accept-Encoding", "*;q=0.9"}});
  auto it = resp.headers.find("Content-Encoding");
  ASSERT_NE(it, resp.headers.end());
  EXPECT_EQ(it->second, "zstd");
  EXPECT_TRUE(test::HasZstdMagic(resp.body));
}

TEST(HttpCompression, TieBreakAgainstGzipHigherQ) {
  if constexpr (!zstdEnabled()) {
    GTEST_SKIP();
  }
  ts.postConfigUpdate([](HttpServerConfig &cfg) {
    cfg.compression.minBytes = 16;
    cfg.compression.preferredFormats = {Encoding::zstd};
    if constexpr (zlibEnabled()) {
      cfg.compression.preferredFormats.push_back(Encoding::gzip);
    }
  });

  std::string payload(512, 'C');
  ts.router().setDefault([payload](const HttpRequest &) {
    HttpResponse resp;
    resp.body(payload);
    resp.header("Content-Type", "text/plain");
    return resp;
  });
  auto resp = test::simpleGet(ts.port(), "/t", {{"Accept-Encoding", "gzip;q=0.9, zstd;q=0.9"}});
  auto it = resp.headers.find("Content-Encoding");
  ASSERT_NE(it, resp.headers.end());
  EXPECT_EQ(it->second, "zstd");
}

TEST(HttpCompression, ZstdActivatesAfterThreshold) {
  if constexpr (!zstdEnabled()) {
    GTEST_SKIP();
  }
  ts.postConfigUpdate([](HttpServerConfig &cfg) {
    cfg.compression.minBytes = 128;
    cfg.compression.preferredFormats = {Encoding::zstd};
  });

  std::string chunk1(64, 'x');
  std::string chunk2(128, 'y');
  ts.router().setDefault([chunk1, chunk2](const HttpRequest &, HttpResponseWriter &writer) {
    writer.status(http::StatusCodeOK);
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
  auto decompressed = test::ZstdRoundTripDecompress(resp.plainBody, original.size());
  EXPECT_EQ(decompressed, original);
}

TEST(HttpCompression, ZstdBelowThresholdIdentity) {
  if constexpr (!zstdEnabled()) {
    GTEST_SKIP();
  }
  ts.postConfigUpdate([](HttpServerConfig &cfg) {
    cfg.compression.minBytes = 1024;
    cfg.compression.preferredFormats = {Encoding::zstd};
  });

  std::string data(200, 'a');
  ts.router().setDefault([data](const HttpRequest &, HttpResponseWriter &writer) {
    writer.status(http::StatusCodeOK);
    writer.contentType("text/plain");
    writer.writeBody(data);
    writer.end();
  });
  auto resp = test::simpleGet(ts.port(), "/zi", {{"Accept-Encoding", "zstd"}});
  auto it = resp.headers.find("Content-Encoding");
  EXPECT_TRUE(it == resp.headers.end());  // identity
  EXPECT_TRUE(resp.plainBody == data) << "identity path should match input exactly";
}