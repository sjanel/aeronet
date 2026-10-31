#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>

#include "aeronet/compression-config.hpp"
#include "aeronet/compression-test-helpers.hpp"
#include "aeronet/direct-compression-mode.hpp"
#include "aeronet/encoding.hpp"
#include "aeronet/features.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request-view.hpp"
#include "aeronet/http-response-writer.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_util.hpp"

#ifdef AERONET_ENABLE_ZSTD
#include <zstd.h>
#endif

using namespace aeronet;

namespace {  // Helper utilities local to this test file

constexpr bool HasGzipMagic(std::string_view body) {
  return body.size() >= 2 && static_cast<unsigned char>(body[0]) == 0x1f && static_cast<unsigned char>(body[1]) == 0x8b;
}

constexpr bool LooksLikeZlib(std::string_view body) {
  // Very loose heuristic: zlib header is 2 bytes: CMF (compression method/flags) + FLG with check bits.
  // CMF lower 4 bits must be 8 (deflate), i.e. 0x78 is common for default window (0x78 0x9C etc).
  return body.size() >= 2 && static_cast<unsigned char>(body[0]) == 0x78;  // ignore second byte variability
}

test::TestServer ts;

}  // namespace

TEST(HttpCompression, BrAppliedWhenEligible) {
  if constexpr (!brotliEnabled()) {
    GTEST_SKIP();
  }
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.compression.minBytes = 32;
    cfg.compression.preferredFormats = {Encoding::br};
  });

  std::string payload(400, 'B');
  ts.router().setDefault([payload](const HttpRequestView&) { return HttpResponse(payload, "text/plain"); });
  auto resp = test::simpleGet(ts.port(), "/br1", {{"Accept-Encoding", "br"}});
  EXPECT_EQ(resp.statusCode, http::StatusCodeOK);
  auto it = resp.headers.find(http::ContentEncoding.get());
  ASSERT_NE(it, resp.headers.end());
  EXPECT_EQ(it->second, "br");
  EXPECT_LT(resp.body.size(), payload.size());
}

TEST(HttpCompression, UserContentEncodingIdentityDisablesCompression) {
  if constexpr (!brotliEnabled()) {
    GTEST_SKIP();
  }
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.compression.minBytes = 1;
    cfg.compression.preferredFormats = {Encoding::br};
  });
  std::string payload(128, 'U');
  ts.router().setDefault([&payload](const HttpRequestView&) {
    HttpResponse respObj;
    respObj.header(http::ContentEncoding, "identity");
    respObj.body(payload, "text/plain");
    return respObj;
  });
  auto resp = test::simpleGet(ts.port(), "/br2", {{"Accept-Encoding", "br"}});
  EXPECT_EQ(resp.statusCode, http::StatusCodeOK);
  auto it = resp.headers.find(http::ContentEncoding.get());
  ASSERT_NE(it, resp.headers.end());
  EXPECT_EQ(it->second, "identity");
  EXPECT_EQ(resp.body, payload);
}

TEST(HttpCompression, BelowThresholdNotCompressed) {
  if constexpr (!brotliEnabled()) {
    GTEST_SKIP();
  }
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.compression.minBytes = 2048;
    cfg.compression.preferredFormats = {Encoding::br};
  });
  std::string smallStr(64, 's');
  ts.router().setDefault([smallStr](const HttpRequestView&) { return HttpResponse(smallStr); });
  auto resp = test::simpleGet(ts.port(), "/br3", {{"Accept-Encoding", "br"}});
  EXPECT_EQ(resp.statusCode, http::StatusCodeOK);
  EXPECT_FALSE(resp.headers.contains(http::ContentEncoding.get()));
  EXPECT_EQ(resp.body.size(), smallStr.size());
}

TEST(HttpCompression, NoAcceptEncodingHeaderStillCompressesDefault) {
  if constexpr (!brotliEnabled()) {
    GTEST_SKIP();
  }
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.compression.minBytes = 16;
    cfg.compression.preferredFormats = {Encoding::br};
  });
  std::string payload(180, 'D');
  ts.router().setDefault([payload](const HttpRequestView&) { return HttpResponse(payload); });
  auto resp = test::simpleGet(ts.port(), "/br4", {{"Accept-Encoding", "*"}});
  EXPECT_EQ(resp.statusCode, http::StatusCodeOK);
  auto it = resp.headers.find(http::ContentEncoding.get());
  ASSERT_NE(it, resp.headers.end());
  EXPECT_EQ(it->second, "br");
}

#if defined(AERONET_ENABLE_BROTLI) || defined(AERONET_ENABLE_ZLIB)
TEST(HttpCompression, PreservesUserContentTypeWhenCompressing) {
  const std::string customType = "application/vnd.acme.resource+json";
  std::string acceptEncoding;
  std::string expectedEncoding;
  if constexpr (brotliEnabled()) {
    expectedEncoding = "br";
    acceptEncoding = "br";
    ts.postConfigUpdate([](HttpServerConfig& cfg) {
      cfg.compression.minBytes = 32;
      cfg.compression.preferredFormats = {Encoding::br};
    });
  } else if constexpr (zlibEnabled()) {
    expectedEncoding = "gzip";
    acceptEncoding = "gzip";
    ts.postConfigUpdate([](HttpServerConfig& cfg) {
      cfg.compression.minBytes = 32;
      cfg.compression.preferredFormats = {Encoding::gzip};
    });
  }

  std::string payload(160, 'R');
  ts.router().setDefault([payload, customType](const HttpRequestView&) { return HttpResponse(payload, customType); });

  auto resp = test::simpleGet(ts.port(), "/ctype", {{"Accept-Encoding", acceptEncoding}});
  EXPECT_EQ(resp.statusCode, http::StatusCodeOK);

  auto itccc = resp.headers.find(http::ContentType.get());
  ASSERT_NE(itccc, resp.headers.end());
  EXPECT_EQ(itccc->second, customType);

  auto it = resp.headers.find(http::ContentEncoding.get());
  ASSERT_NE(it, resp.headers.end());
  EXPECT_EQ(it->second, expectedEncoding);
  EXPECT_LT(resp.body.size(), payload.size());
}
#endif  // defined(AERONET_ENABLE_BROTLI) || defined(AERONET_ENABLE_ZLIB)

TEST(HttpCompression, InlineBodyCompressionMovesToCapturedPayload) {
#if !defined(AERONET_ENABLE_BROTLI) && !defined(AERONET_ENABLE_ZLIB)
  GTEST_SKIP();
#endif

  std::string expectedEncoding;
  std::string acceptEncoding;

#ifdef AERONET_ENABLE_BROTLI
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.compression.minBytes = 64;
    cfg.compression.preferredFormats = {Encoding::br};
  });
  expectedEncoding = "br";
  acceptEncoding = "br";
#elifdef AERONET_ENABLE_ZLIB
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.compression.minBytes = 64;
    cfg.compression.preferredFormats = {Encoding::gzip};
  });
  expectedEncoding = "gzip";
  acceptEncoding = "gzip";
#endif

  const std::string customType = "application/x-inline";
  std::string inlinePayload(512, 'I');
  ts.router().setDefault(
      [inlinePayload, customType](const HttpRequestView&) { return HttpResponse(inlinePayload, customType); });

  auto resp = test::simpleGet(ts.port(), "/inline", {{"Accept-Encoding", acceptEncoding}});
  EXPECT_EQ(resp.statusCode, http::StatusCodeOK);
  auto it = resp.headers.find(http::ContentEncoding.get());
  ASSERT_NE(it, resp.headers.end());
  EXPECT_EQ(it->second, expectedEncoding);

  auto itType = resp.headers.find(http::ContentType.get());
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
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
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
  ts.router().setDefault([payload](const HttpRequestView&) {
    HttpResponse respObj;
    // supply body as captured payload directly (simulate handler that sets captured payload)
    respObj.body(payload);
    // add trailers after body
    respObj.trailerAddLine("x-checksum", "cksum");
    respObj.trailerAddLine("x-extra", "val");
    return respObj;
  });

  test::ClientConnection sock(ts.port());
  NativeHandle fd = sock.fd();
  std::string req =
      "GET /captured-trailers HTTP/1.1\r\n"
      "host: example.com\r\n"
      "connection: close\r\n"
      "accept-encoding: *\r\n"
      "\r\n";
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);
  EXPECT_TRUE(resp.contains("x-checksum: cksum"));
  EXPECT_TRUE(resp.contains("x-extra: val"));
}

// Compression for inline body where compression moves body to captured payload, with trailers added
TEST(HttpCompression, InlineBodyWithTrailers) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
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
  ts.router().setDefault([inlinePayload](const HttpRequestView&) {
    HttpResponse respObj;
    // create inline body (string_view) to force inline storage
    respObj.body(std::string_view(inlinePayload));
    // trailers must be added after body
    respObj.trailerAddLine("x-inline", "ok");
    return respObj;
  });

  test::ClientConnection sock(ts.port());
  NativeHandle fd = sock.fd();
  std::string req =
      "GET /inline-trailers HTTP/1.1\r\n"
      "host: example.com\r\n"
      "connection: close\r\n"
      "accept-encoding: *\r\n"
      "\r\n";
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);
  EXPECT_TRUE(resp.contains("x-inline: ok"));
}

TEST(HttpCompression, IdentityForbiddenNoAlternativesReturns406) {
  if constexpr (!brotliEnabled()) {
    GTEST_SKIP();
  }
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.compression.minBytes = 1;
    cfg.compression.preferredFormats = {Encoding::br};
  });
  std::string payload(70, 'Q');
  ts.router().setDefault([payload](const HttpRequestView&) { return HttpResponse(payload); });
  auto resp = test::simpleGet(ts.port(), "/br5", {{"Accept-Encoding", "identity;q=0, snappy;q=0"}});
  EXPECT_EQ(resp.statusCode, http::StatusCodeNotAcceptable);
  EXPECT_EQ(resp.body, "No acceptable content-coding available");
}

TEST(HttpCompression, BrActivatedOverThreshold) {
  if constexpr (!brotliEnabled()) {
    GTEST_SKIP();
  }
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.compression.minBytes = 64;
    cfg.compression.preferredFormats = {Encoding::br};
  });
  std::string part1(40, 'a');
  std::string part2(80, 'b');
  ts.router().setDefault([part1, part2](const HttpRequestView&, HttpResponseWriter& writer) {
    writer.status(http::StatusCodeOK);
    writer.writeBody(part1);
    writer.writeBody(part2);
    writer.end();
  });
  auto resp = test::simpleGet(ts.port(), "/sbr1", {{"Accept-Encoding", "br"}});
  auto it = resp.headers.find(http::ContentEncoding.get());
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
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.compression.minBytes = 1024;
    cfg.compression.preferredFormats = {Encoding::br};
  });
  std::string smallStr(80, 'x');
  ts.router().setDefault([smallStr](const HttpRequestView&) { return HttpResponse(smallStr); });
  auto resp = test::simpleGet(ts.port(), "/sbr2", {{"Accept-Encoding", "br"}});
  EXPECT_FALSE(resp.headers.contains(http::ContentEncoding.get()));
  EXPECT_TRUE(resp.body.contains('x'));
}

TEST(HttpCompression, UserProvidedIdentityPreventsActivation) {
  if constexpr (!brotliEnabled()) {
    GTEST_SKIP();
  }
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.compression.minBytes = 16;
    cfg.compression.preferredFormats = {Encoding::br};
  });
  std::string payload(512, 'Y');
  ts.router().setDefault([payload]([[maybe_unused]] const HttpRequestView& req, HttpResponseWriter& writer) {
    writer.status(http::StatusCodeOK);
    writer.header(http::ContentEncoding, "identity");
    writer.writeBody(payload);
    writer.end();
  });
  auto resp = test::simpleGet(ts.port(), "/sbr3", {{"Accept-Encoding", "br"}});
  auto it = resp.headers.find(http::ContentEncoding.get());
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
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.compression.minBytes = 64;
    cfg.compression.preferredFormats = {Encoding::br};
  });
  std::string payload(600, 'Z');
  ts.router().setDefault([payload]([[maybe_unused]] const HttpRequestView& req, HttpResponseWriter& writer) {
    writer.status(http::StatusCodeOK);
    writer.writeBody(payload.substr(0, 128));
    writer.writeBody(payload.substr(128));
    writer.end();
  });
  // Client strongly prefers br
  auto resp = test::simpleGet(ts.port(), "/sbr4", {{"Accept-Encoding", "gzip;q=0.5, br;q=1.0"}});
  auto it = resp.headers.find(http::ContentEncoding.get());
  if (it != resp.headers.end()) {
    EXPECT_EQ(it->second, "br");
  }
}

TEST(HttpCompression, StreamingIdentityForbiddenNoAlternativesReturns406) {
  if constexpr (!brotliEnabled()) {
    GTEST_SKIP();
  }
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.compression.minBytes = 1;
    cfg.compression.preferredFormats = {Encoding::br};
  });
  std::string payload(90, 'F');
  ts.router().setDefault([payload]([[maybe_unused]] const HttpRequestView& req, HttpResponseWriter& writer) {
    writer.status(http::StatusCodeOK);
    writer.writeBody(payload);
    writer.end();
  });
  auto resp = test::simpleGet(ts.port(), "/sbr5", {{"Accept-Encoding", "identity;q=0, snappy;q=0"}});
  // Server should respond 406 (not compressible with offered encodings; identity forbidden)
  EXPECT_TRUE(resp.headersRaw.starts_with("HTTP/1.1 406"));
}

TEST(HttpCompression, GzipUserContentEncodingIdentityDisablesCompression) {
  if constexpr (!zlibEnabled()) {
    GTEST_SKIP();
  }
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.compression.minBytes = 1;
    cfg.compression.preferredFormats = {Encoding::gzip};
  });
  std::string payload(128, 'B');
  ts.router().setDefault([payload](const HttpRequestView&) {
    HttpResponse resp;
    resp.header(http::ContentEncoding, "identity");  // explicit suppression
    resp.body(payload, "text/plain");
    return resp;
  });
  auto resp = test::simpleGet(ts.port(), "/o", {{"Accept-Encoding", "gzip"}});
  EXPECT_EQ(resp.statusCode, http::StatusCodeOK);
  // Should remain uncompressed and server must not alter user-provided identity
  auto itCE = resp.headers.find(http::ContentEncoding.get());
  ASSERT_NE(itCE, resp.headers.end());
  EXPECT_EQ(itCE->second, "identity");
  EXPECT_EQ(resp.body.size(), payload.size());
}

TEST(HttpCompression, GzipBelowThresholdNotCompressed) {
  if constexpr (!zlibEnabled()) {
    GTEST_SKIP();
  }
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.compression.minBytes = 1024;
    cfg.compression.preferredFormats = {Encoding::gzip};
  });
  std::string smallPayload(32, 'C');
  ts.router().setDefault([smallPayload](const HttpRequestView&) { return HttpResponse(smallPayload, "text/plain"); });
  auto resp = test::simpleGet(ts.port(), "/s", {{"Accept-Encoding", "gzip"}});
  EXPECT_EQ(resp.statusCode, http::StatusCodeOK);
  EXPECT_FALSE(resp.headers.contains(http::ContentEncoding.get()));
  EXPECT_EQ(resp.body.size(), smallPayload.size());
}

TEST(HttpCompression, GzipNoAcceptEncodingHeaderStillCompressesDefault) {
  if constexpr (!zlibEnabled()) {
    GTEST_SKIP();
  }
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.compression.minBytes = 16;
    cfg.compression.preferredFormats = {Encoding::gzip};
  });
  std::string payload(128, 'D');
  ts.router().setDefault([payload](const HttpRequestView&) { return HttpResponse(payload, "text/plain"); });
  auto resp = test::simpleGet(ts.port(), "/i", {});
  EXPECT_EQ(resp.statusCode, http::StatusCodeOK);
  auto it = resp.headers.find(http::ContentEncoding.get());
  if (it != resp.headers.end()) {
    EXPECT_EQ(it->second, "gzip");
    EXPECT_TRUE(HasGzipMagic(resp.body));
  }
}

TEST(HttpCompression, GzipIdentityForbiddenNoAlternativesReturns406) {
  if constexpr (!zlibEnabled()) {
    GTEST_SKIP();
  }
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.compression.minBytes = 1;
    cfg.compression.preferredFormats = {Encoding::gzip};
  });
  std::string payload(64, 'Q');
  ts.router().setDefault([payload](const HttpRequestView&) { return HttpResponse(payload); });
  // Client forbids identity and offers only unsupported encodings (br here is unsupported in current build).
  auto resp = test::simpleGet(ts.port(), "/bad", {{"Accept-Encoding", "identity;q=0, br;q=0"}});
  EXPECT_EQ(resp.statusCode, http::StatusCodeNotAcceptable);
  EXPECT_EQ(resp.body, "No acceptable content-coding available");
}

TEST(HttpCompression, IdentityForbiddenButGzipAvailableUsesGzip) {
  if constexpr (!zlibEnabled()) {
    GTEST_SKIP();
  }
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.compression.minBytes = 1;
    cfg.compression.preferredFormats = {Encoding::gzip};
  });
  std::string payload(128, 'Z');
  ts.router().setDefault([payload](const HttpRequestView&) { return HttpResponse(payload); });
  auto resp = test::simpleGet(ts.port(), "/ok", {{"Accept-Encoding", "identity;q=0, gzip"}});
  EXPECT_EQ(resp.statusCode, http::StatusCodeOK);
  auto it = resp.headers.find(http::ContentEncoding.get());
  ASSERT_NE(it, resp.headers.end());
  EXPECT_EQ(it->second, "gzip");
  EXPECT_TRUE(HasGzipMagic(resp.body));
}

TEST(HttpCompression, UnsupportedEncodingDoesNotApplyGzip) {
  if constexpr (!zlibEnabled()) {
    GTEST_SKIP();
  }
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.compression.minBytes = 1;
    cfg.compression.preferredFormats = {Encoding::gzip};
  });
  std::string payload(200, 'E');
  ts.router().setDefault([payload](const HttpRequestView&) { return HttpResponse(payload); });
  // If brotli support is compiled in, 'br' is actually supported and would trigger compression.
  // Use an obviously unsupported token (snappy) in that case.
  auto resp = test::simpleGet(ts.port(), "/br", {{"Accept-Encoding", brotliEnabled() ? "snappy" : "br"}});
  EXPECT_EQ(resp.statusCode, http::StatusCodeOK);
  EXPECT_EQ(resp.headers.find(http::ContentEncoding.get()), resp.headers.end());
}

TEST(HttpCompression, DeflateAppliedWhenPreferredAndAccepted) {
  if constexpr (!zlibEnabled()) {
    GTEST_SKIP();
  }
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.compression.minBytes = 32;
    cfg.compression.preferredFormats = {Encoding::deflate, Encoding::gzip};
  });
  std::string largePayload(300, 'F');
  ts.router().setDefault([largePayload](const HttpRequestView&) { return HttpResponse(largePayload); });
  auto resp = test::simpleGet(ts.port(), "/d1", {{"Accept-Encoding", "deflate,gzip"}});
  EXPECT_EQ(resp.statusCode, 200);
  auto it = resp.headers.find(http::ContentEncoding.get());
  ASSERT_NE(it, resp.headers.end());
  EXPECT_EQ(it->second, "deflate");
  EXPECT_TRUE(LooksLikeZlib(resp.body));
  EXPECT_LT(resp.body.size(), largePayload.size());
}

TEST(HttpCompression, GzipChosenWhenHigherPreference) {
  if constexpr (!zlibEnabled()) {
    GTEST_SKIP();
  }
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.compression.minBytes = 16;
    cfg.compression.preferredFormats = {Encoding::gzip, Encoding::deflate};
  });
  std::string payload(256, 'G');
  ts.router().setDefault([payload](const HttpRequestView&) { return HttpResponse(payload); });
  auto resp = test::simpleGet(ts.port(), "/d2", {{"Accept-Encoding", "gzip,deflate"}});
  auto it = resp.headers.find(http::ContentEncoding.get());
  ASSERT_NE(it, resp.headers.end());
  EXPECT_EQ(it->second, "gzip");
  EXPECT_TRUE(HasGzipMagic(resp.body));
}

TEST(HttpCompression, QValuesAffectSelection) {
  if constexpr (!zlibEnabled()) {
    GTEST_SKIP();
  }
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.compression.minBytes = 16;
    cfg.compression.preferredFormats = {Encoding::gzip, Encoding::deflate};
  });
  std::string payload(180, 'H');
  ts.router().setDefault([payload](const HttpRequestView&) { return HttpResponse(payload); });
  auto resp = test::simpleGet(ts.port(), "/d3", {{"Accept-Encoding", "gzip;q=0.1, deflate;q=0.9"}});
  auto it = resp.headers.find(http::ContentEncoding.get());
  ASSERT_NE(it, resp.headers.end());
  EXPECT_EQ(it->second, "deflate");
  EXPECT_TRUE(LooksLikeZlib(resp.body));
}

TEST(HttpCompression, IdentityFallbackIfDeflateNotRequested) {
  if constexpr (!zlibEnabled()) {
    GTEST_SKIP();
  }
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.compression.minBytes = 8;
    cfg.compression.preferredFormats = {Encoding::deflate};
  });
  std::string payload(256, 'I');
  ts.router().setDefault([payload](const HttpRequestView&) { return HttpResponse(payload); });
  auto resp = test::simpleGet(ts.port(), "/d4", {{"Accept-Encoding", "gzip"}});  // client does NOT list deflate
  auto it = resp.headers.find(http::ContentEncoding.get());
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
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.compression.minBytes = 64;
    cfg.compression.preferredFormats = {Encoding::gzip};
  });
  std::string part1(40, 'a');
  std::string part2(80, 'b');
  ts.router().setDefault([part1, part2](const HttpRequestView&, HttpResponseWriter& writer) {
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
  auto it = resp.headers.find(http::ContentEncoding.get());
  if (it != resp.headers.end()) {
    EXPECT_EQ(it->second, "gzip");
  }
  EXPECT_TRUE(resp.body.contains("\x1f\x8b") || HasGzipMagic(resp.body));
}

TEST(HttpCompression, StreamingDeflateActivatedOverThreshold) {
  if constexpr (!zlibEnabled()) {
    GTEST_SKIP();
  }
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.compression.minBytes = 32;
    cfg.compression.preferredFormats = {Encoding::deflate, Encoding::gzip};
  });
  std::string payload(128, 'X');
  ts.router().setDefault([payload](const HttpRequestView&, HttpResponseWriter& writer) {
    writer.status(http::StatusCodeOK);
    writer.writeBody(payload.substr(0, 40));
    writer.writeBody(payload.substr(40));
    writer.end();
  });
  auto resp = test::simpleGet(ts.port(), "/sdf", {{"Accept-Encoding", "deflate,gzip"}});
  auto it = resp.headers.find(http::ContentEncoding.get());
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
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.compression.minBytes = 512;
    cfg.compression.preferredFormats = {Encoding::gzip};
  });

  std::string smallStr(40, 'y');
  ts.router().setDefault([smallStr](const HttpRequestView&, HttpResponseWriter& writer) {
    writer.status(http::StatusCodeOK);
    writer.writeBody(smallStr);  // never crosses threshold
    writer.end();
  });
  auto resp = test::simpleGet(ts.port(), "/sid", {{"Accept-Encoding", "gzip"}});
  EXPECT_FALSE(resp.headers.contains(http::ContentEncoding.get()));
  EXPECT_TRUE(resp.body.contains(smallStr));
}

#if defined(AERONET_ENABLE_BROTLI) || defined(AERONET_ENABLE_ZLIB)
TEST(HttpCompression, StreamingUserProvidedContentEncodingIdentityPreventsActivation) {
  std::string expectedEncoding;
  std::string acceptEncoding;
  if constexpr (brotliEnabled()) {
    expectedEncoding = "br";
    acceptEncoding = "br";
    ts.postConfigUpdate([](HttpServerConfig& cfg) {
      cfg.compression.minBytes = 16;
      cfg.compression.preferredFormats = {Encoding::br};
    });
  } else if constexpr (zlibEnabled()) {
    expectedEncoding = "gzip";
    acceptEncoding = "gzip";
    ts.postConfigUpdate([](HttpServerConfig& cfg) {
      cfg.compression.minBytes = 16;
      cfg.compression.preferredFormats = {Encoding::gzip};
    });
  }

  std::string big(200, 'Z');
  ts.router().setDefault([big](const HttpRequestView&, HttpResponseWriter& writer) {
    writer.status(http::StatusCodeOK);
    writer.header(http::ContentEncoding, "identity");  // explicit suppression
    writer.writeBody(big.substr(0, 50));
    writer.writeBody(big.substr(50));
    writer.end();
  });
  auto resp = test::simpleGet(ts.port(), "/soff", {{"Accept-Encoding", acceptEncoding}});
  auto it = resp.headers.find(http::ContentEncoding.get());
  ASSERT_NE(it, resp.headers.end());
  EXPECT_EQ(it->second, "identity");
  // Body should contain literal 'Z' sequences (chunked framing around them)
  EXPECT_TRUE(resp.body.contains('Z'));
}
#endif  // defined(AERONET_ENABLE_BROTLI) || defined(AERONET_ENABLE_ZLIB)

#ifdef AERONET_ENABLE_ZLIB
TEST(HttpCompression, StreamingQValuesInfluenceStreamingSelection) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.compression.minBytes = 16;
    cfg.compression.preferredFormats = {Encoding::gzip, Encoding::deflate};
  });
  std::string payload(180, 'Q');
  ts.router().setDefault([payload](const HttpRequestView&, HttpResponseWriter& writer) {
    writer.status(http::StatusCodeOK);
    writer.contentType("text/plain");
    writer.writeBody(payload.substr(0, 60));
    writer.writeBody(payload.substr(60));
    writer.end();
  });
  auto resp = test::simpleGet(ts.port(), "/sqv", {{"Accept-Encoding", "gzip;q=0.1, deflate;q=0.9"}});
  auto it = resp.headers.find(http::ContentEncoding.get());
  ASSERT_NE(it, resp.headers.end());
  EXPECT_EQ(it->second, "deflate");
}
#endif

#if defined(AERONET_ENABLE_BROTLI) || defined(AERONET_ENABLE_ZLIB)
TEST(HttpCompression, GzipStreamingIdentityForbiddenNoAlternativesReturns406) {
  if constexpr (brotliEnabled()) {
    ts.postConfigUpdate([](HttpServerConfig& cfg) {
      cfg.compression.minBytes = 1;
      cfg.compression.preferredFormats = {Encoding::br};
    });
  } else if constexpr (zlibEnabled()) {
    ts.postConfigUpdate([](HttpServerConfig& cfg) {
      cfg.compression.minBytes = 1;
      cfg.compression.preferredFormats = {Encoding::gzip};
    });
  }

  std::atomic<bool> handlerInvoked{false};
  ts.router().setDefault([&handlerInvoked](const HttpRequestView&, HttpResponseWriter& writer) {
    handlerInvoked = true;
    writer.status(http::StatusCodeOK);  // will be overridden to 406 before handler invoked if negotiation rejects
    writer.contentType("text/plain");
    writer.writeBody(std::string(64, 'Q'));
    writer.end();
  });
  auto resp = test::simpleGet(ts.port(), "/sbad", {{"Accept-Encoding", "identity;q=0, br;q=0"}});
  EXPECT_TRUE(resp.headersRaw.rfind("HTTP/1.1 406", 0) == 0) << resp.headersRaw;
  EXPECT_EQ(resp.body, "No acceptable content-coding available");
  EXPECT_FALSE(handlerInvoked.load());
}
#endif  // defined(AERONET_ENABLE_BROTLI) || defined(AERONET_ENABLE_ZLIB)

#ifdef AERONET_ENABLE_ZSTD
TEST(HttpCompression, ZstdAppliedWhenEligible) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.compression.minBytes = 32;
    cfg.compression.preferredFormats = {Encoding::zstd};
  });
  std::string payload(400, 'A');
  ts.router().setDefault([payload](const HttpRequestView&) { return HttpResponse(payload); });
  auto resp = test::simpleGet(ts.port(), "/z", {{"Accept-Encoding", "zstd"}});
  ASSERT_EQ(resp.statusCode, 200);
  auto it = resp.headers.find(http::ContentEncoding.get());
  ASSERT_NE(it, resp.headers.end());
  EXPECT_EQ(it->second, "zstd");
  EXPECT_TRUE(test::HasZstdMagic(resp.body));
  EXPECT_LT(resp.body.size(), payload.size());
  // Round-trip verify by decompressing (simple one-shot) to ensure integrity
  std::string decompressed = test::ZstdRoundTripDecompress(resp.body, payload.size());
  EXPECT_EQ(decompressed, payload);
}

TEST(HttpCompression, WildcardSelectsZstdIfPreferred) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.compression.minBytes = 16;
    cfg.compression.preferredFormats = {Encoding::zstd};
    if constexpr (zlibEnabled()) {
      cfg.compression.preferredFormats.push_back(Encoding::gzip);
    }
  });

  std::string payload(256, 'B');
  ts.router().setDefault([payload](const HttpRequestView&) { return HttpResponse(payload); });
  auto resp = test::simpleGet(ts.port(), "/w", {{"Accept-Encoding", "*;q=0.9"}});
  auto it = resp.headers.find(http::ContentEncoding.get());
  ASSERT_NE(it, resp.headers.end());
  EXPECT_EQ(it->second, "zstd");
  EXPECT_TRUE(test::HasZstdMagic(resp.body));
}

TEST(HttpCompression, TieBreakAgainstGzipHigherQ) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.compression.minBytes = 16;
    cfg.compression.preferredFormats = {Encoding::zstd};
    if constexpr (zlibEnabled()) {
      cfg.compression.preferredFormats.push_back(Encoding::gzip);
    }
  });

  std::string payload(512, 'C');
  ts.router().setDefault([payload](const HttpRequestView&) { return HttpResponse(payload); });
  auto resp = test::simpleGet(ts.port(), "/t", {{"Accept-Encoding", "gzip;q=0.9, zstd;q=0.9"}});
  auto it = resp.headers.find(http::ContentEncoding.get());
  ASSERT_NE(it, resp.headers.end());
  EXPECT_EQ(it->second, "zstd");
}

TEST(HttpCompression, ZstdActivatesAfterThreshold) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.compression.minBytes = 128;
    cfg.compression.preferredFormats = {Encoding::zstd};
  });

  std::string chunk1(64, 'x');
  std::string chunk2(128, 'y');
  ts.router().setDefault([chunk1, chunk2](const HttpRequestView&, HttpResponseWriter& writer) {
    writer.status(http::StatusCodeOK);
    writer.contentType("text/plain");
    writer.writeBody(chunk1);
    writer.writeBody(chunk2);
    writer.end();
  });
  auto resp = test::simpleGet(ts.port(), "/zs", {{"Accept-Encoding", "zstd"}});
  auto it = resp.headers.find(http::ContentEncoding.get());
  ASSERT_NE(it, resp.headers.end());
  EXPECT_EQ(it->second, "zstd");
  EXPECT_TRUE(test::HasZstdMagic(resp.plainBody));
  // Round-trip decompression via helper
  std::string original = chunk1 + chunk2;
  auto decompressed = test::ZstdRoundTripDecompress(resp.plainBody, original.size());
  EXPECT_EQ(decompressed, original);
}

TEST(HttpCompression, ZstdBelowThresholdIdentity) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.compression.minBytes = 1024;
    cfg.compression.preferredFormats = {Encoding::zstd};
  });

  std::string data(200, 'a');
  ts.router().setDefault([data](const HttpRequestView&, HttpResponseWriter& writer) {
    writer.status(http::StatusCodeOK);
    writer.contentType("text/plain");
    writer.writeBody(data);
    writer.end();
  });
  auto resp = test::simpleGet(ts.port(), "/zi", {{"Accept-Encoding", "zstd"}});
  auto it = resp.headers.find(http::ContentEncoding.get());
  EXPECT_TRUE(it == resp.headers.end());  // identity
  EXPECT_TRUE(resp.plainBody == data) << "identity path should match input exactly";
}
#endif

// =============================================================================
// Direct Compression (inline body compressed at body-set time via req.makeResponse())
// =============================================================================

#ifdef AERONET_ENABLE_ZLIB
TEST(HttpCompression, DirectCompression_GzipRoundTrip) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.compression.minBytes = 32;
    cfg.compression.defaultDirectCompressionMode = DirectCompressionMode::Auto;
    cfg.compression.preferredFormats = {Encoding::gzip};
  });

  std::string payload(512, 'G');
  ts.router().setDefault([&payload](const HttpRequestView& req) {
    auto resp = req.makeResponse();
    resp.body(std::string_view{payload}, "text/plain");
    return resp;
  });

  auto resp = test::simpleGet(ts.port(), "/dc-gz", {{"Accept-Encoding", "gzip"}});
  EXPECT_EQ(resp.statusCode, http::StatusCodeOK);
  auto it = resp.headers.find(http::ContentEncoding.get());
  ASSERT_NE(it, resp.headers.end());
  EXPECT_EQ(it->second, "gzip");
  EXPECT_LT(resp.body.size(), payload.size());

  // Round-trip decompression
  auto decompressed = test::Decompress(Encoding::gzip, resp.body);
  EXPECT_EQ(std::string_view(decompressed.data(), decompressed.size()), payload);
}

TEST(HttpCompression, DirectCompression_DeflateRoundTrip) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.compression.minBytes = 32;
    cfg.compression.defaultDirectCompressionMode = DirectCompressionMode::Auto;
    cfg.compression.preferredFormats = {Encoding::deflate};
  });

  std::string payload(512, 'D');
  ts.router().setDefault([&payload](const HttpRequestView& req) {
    auto resp = req.makeResponse();
    resp.body(std::string_view{payload}, "text/plain");
    return resp;
  });

  auto resp = test::simpleGet(ts.port(), "/dc-df", {{"Accept-Encoding", "deflate"}});
  EXPECT_EQ(resp.statusCode, http::StatusCodeOK);
  auto it = resp.headers.find(http::ContentEncoding.get());
  ASSERT_NE(it, resp.headers.end());
  EXPECT_EQ(it->second, "deflate");
  EXPECT_LT(resp.body.size(), payload.size());

  auto decompressed = test::Decompress(Encoding::deflate, resp.body);
  EXPECT_EQ(std::string_view(decompressed.data(), decompressed.size()), payload);
}

TEST(HttpCompression, DirectCompression_ModeOff_StillCompressedByFinalization) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.compression.minBytes = 32;
    cfg.compression.defaultDirectCompressionMode = DirectCompressionMode::Off;
    cfg.compression.preferredFormats = {Encoding::gzip};
  });

  std::string payload(512, 'F');
  ts.router().setDefault([&payload](const HttpRequestView& req) {
    auto resp = req.makeResponse();
    resp.body(std::string_view{payload}, "text/plain");
    return resp;
  });

  auto resp = test::simpleGet(ts.port(), "/dc-off", {{"Accept-Encoding", "gzip"}});
  EXPECT_EQ(resp.statusCode, http::StatusCodeOK);
  // Even with DC off, finalization layer should apply compression
  auto it = resp.headers.find(http::ContentEncoding.get());
  ASSERT_NE(it, resp.headers.end());
  EXPECT_EQ(it->second, "gzip");

  auto decompressed = test::Decompress(Encoding::gzip, resp.body);
  EXPECT_EQ(std::string_view(decompressed.data(), decompressed.size()), payload);
}

TEST(HttpCompression, DirectCompression_ModeOn_SmallBodyCompressed) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.compression.minBytes = 4096;  // high threshold
    cfg.compression.defaultDirectCompressionMode = DirectCompressionMode::Auto;
    cfg.compression.preferredFormats = {Encoding::gzip};
  });

  std::string payload(128, 'S');  // well below minBytes
  ts.router().setDefault([&payload](const HttpRequestView& req) {
    auto resp = req.makeResponse();
    resp.directCompressionMode(DirectCompressionMode::On);  // force direct compression
    resp.body(std::string_view{payload}, "text/plain");
    return resp;
  });

  auto resp = test::simpleGet(ts.port(), "/dc-on", {{"Accept-Encoding", "gzip"}});
  EXPECT_EQ(resp.statusCode, http::StatusCodeOK);
  auto it = resp.headers.find(http::ContentEncoding.get());
  ASSERT_NE(it, resp.headers.end());
  EXPECT_EQ(it->second, "gzip");
  EXPECT_LT(resp.body.size(), payload.size());

  auto decompressed = test::Decompress(Encoding::gzip, resp.body);
  EXPECT_EQ(std::string_view(decompressed.data(), decompressed.size()), payload);
}

TEST(HttpCompression, DirectCompression_BodyAppendGzipStreaming) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.compression.minBytes = 32;
    cfg.compression.defaultDirectCompressionMode = DirectCompressionMode::Auto;
    cfg.compression.preferredFormats = {Encoding::gzip};
  });

  std::string chunk1(256, 'A');
  std::string chunk2(256, 'B');
  ts.router().setDefault([&chunk1, &chunk2](const HttpRequestView& req) {
    auto resp = req.makeResponse();
    resp.body(std::string_view{chunk1}, "text/plain");
    resp.bodyAppend(std::string_view{chunk2});
    return resp;
  });

  auto resp = test::simpleGet(ts.port(), "/dc-append", {{"Accept-Encoding", "gzip"}});
  EXPECT_EQ(resp.statusCode, http::StatusCodeOK);
  auto it = resp.headers.find(http::ContentEncoding.get());
  ASSERT_NE(it, resp.headers.end());
  EXPECT_EQ(it->second, "gzip");

  auto decompressed = test::Decompress(Encoding::gzip, resp.body);
  std::string expected = chunk1 + chunk2;
  EXPECT_EQ(std::string_view(decompressed.data(), decompressed.size()), expected);
}

TEST(HttpCompression, DirectCompression_BodyResetDeliversFinalContent) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.compression.minBytes = 32;
    cfg.compression.defaultDirectCompressionMode = DirectCompressionMode::Auto;
    cfg.compression.preferredFormats = {Encoding::gzip};
  });

  std::string firstPayload(256, '1');
  std::string secondPayload(256, '2');
  ts.router().setDefault([&firstPayload, &secondPayload](const HttpRequestView& req) {
    auto resp = req.makeResponse();
    resp.body(std::string_view{firstPayload}, "text/plain");  // direct-compressed
    resp.body(std::string_view{secondPayload});               // reset: re-initiates compression with new data
    return resp;
  });

  auto resp = test::simpleGet(ts.port(), "/dc-reset", {{"Accept-Encoding", "gzip"}});
  EXPECT_EQ(resp.statusCode, http::StatusCodeOK);
  auto it = resp.headers.find(http::ContentEncoding.get());
  ASSERT_NE(it, resp.headers.end());
  EXPECT_EQ(it->second, "gzip");

  auto decompressed = test::Decompress(Encoding::gzip, resp.body);
  EXPECT_EQ(std::string_view(decompressed.data(), decompressed.size()), secondPayload);
}

TEST(HttpCompression, DirectCompression_VaryHeaderPresent) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.compression.minBytes = 32;
    cfg.compression.addVaryAcceptEncodingHeader = true;
    cfg.compression.defaultDirectCompressionMode = DirectCompressionMode::Auto;
    cfg.compression.preferredFormats = {Encoding::gzip};
  });

  std::string payload(256, 'V');
  ts.router().setDefault([&payload](const HttpRequestView& req) {
    auto resp = req.makeResponse();
    resp.body(std::string_view{payload});
    return resp;
  });

  auto resp = test::simpleGet(ts.port(), "/dc-vary", {{"Accept-Encoding", "gzip"}});
  EXPECT_EQ(resp.statusCode, http::StatusCodeOK);
  auto itVary = resp.headers.find(http::Vary.get());
  ASSERT_NE(itVary, resp.headers.end());
  EXPECT_TRUE(itVary->second.contains("accept-encoding") || itVary->second.contains("Accept-Encoding"))
      << "Vary header = " << itVary->second;
}

TEST(HttpCompression, DirectCompression_UserContentEncodingPrevents) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.compression.minBytes = 32;
    cfg.compression.defaultDirectCompressionMode = DirectCompressionMode::Auto;
    cfg.compression.preferredFormats = {Encoding::gzip};
  });

  std::string payload(256, 'I');
  ts.router().setDefault([&payload](const HttpRequestView& req) {
    auto resp = req.makeResponse();
    resp.header(http::ContentEncoding, "identity");  // user sets Content-Encoding
    resp.body(std::string_view{payload});
    return resp;
  });

  auto resp = test::simpleGet(ts.port(), "/dc-uce", {{"Accept-Encoding", "gzip"}});
  EXPECT_EQ(resp.statusCode, http::StatusCodeOK);
  auto it = resp.headers.find(http::ContentEncoding.get());
  ASSERT_NE(it, resp.headers.end());
  EXPECT_EQ(it->second, "identity");
  EXPECT_EQ(resp.body, payload);  // not compressed
}

TEST(HttpCompression, DirectCompression_ContentTypeAllowList) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.compression.minBytes = 32;
    cfg.compression.defaultDirectCompressionMode = DirectCompressionMode::Auto;
    cfg.compression.preferredFormats = {Encoding::gzip};
    cfg.compression.contentTypeAllowList.clear();
    cfg.compression.contentTypeAllowList.append("application/json");
  });

  std::string payload(256, 'X');

  // text/plain is NOT in allow list → should not compress
  ts.router().setDefault([&payload](const HttpRequestView& req) {
    auto resp = req.makeResponse();
    resp.body(std::string_view{payload});
    return resp;
  });

  auto resp = test::simpleGet(ts.port(), "/dc-deny", {{"Accept-Encoding", "gzip"}});
  EXPECT_EQ(resp.statusCode, http::StatusCodeOK);
  EXPECT_FALSE(resp.headers.contains(http::ContentEncoding.get()));
  EXPECT_EQ(resp.body, payload);

  // application/json IS in allow list → should compress
  ts.router().setDefault([&payload](const HttpRequestView& req) {
    auto resp = req.makeResponse();
    resp.body(std::string_view{payload}, "application/json");
    return resp;
  });

  auto resp2 = test::simpleGet(ts.port(), "/dc-allow", {{"Accept-Encoding", "gzip"}});
  EXPECT_EQ(resp2.statusCode, http::StatusCodeOK);
  auto it = resp2.headers.find(http::ContentEncoding.get());
  ASSERT_NE(it, resp2.headers.end());
  EXPECT_EQ(it->second, "gzip");

  // Cleanup
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.compression.contentTypeAllowList.clear(); });
}

TEST(HttpCompression, DirectCompression_WithTrailers) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.compression.minBytes = 32;
    cfg.compression.defaultDirectCompressionMode = DirectCompressionMode::Auto;
    cfg.compression.preferredFormats = {Encoding::gzip};
  });

  std::string payload(256, 'T');
  ts.router().setDefault([&payload](const HttpRequestView& req) {
    auto resp = req.makeResponse();
    resp.body(std::string_view{payload}, "text/plain");
    resp.trailerAddLine("x-checksum", "abc123");
    return resp;
  });

  test::ClientConnection sock(ts.port());
  NativeHandle fd = sock.fd();
  std::string req =
      "GET /dc-trailers HTTP/1.1\r\n"
      "host: example.com\r\n"
      "connection: close\r\n"
      "accept-encoding: gzip\r\n"
      "\r\n";
  test::sendAll(fd, req);
  std::string rawResp = test::recvUntilClosed(fd);
  EXPECT_TRUE(rawResp.contains("x-checksum: abc123"));
}

TEST(HttpCompression, DirectCompression_MultipleSequentialRequests) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.compression.minBytes = 32;
    cfg.compression.defaultDirectCompressionMode = DirectCompressionMode::Auto;
    cfg.compression.preferredFormats = {Encoding::gzip};
  });

  std::string payload(256, 'M');
  ts.router().setDefault([&payload](const HttpRequestView& req) {
    auto resp = req.makeResponse();
    resp.body(std::string_view{payload}, "text/plain");
    return resp;
  });

  // Send two sequential requests and verify both are correctly compressed
  for (int ii = 0; ii < 2; ++ii) {
    auto resp = test::simpleGet(ts.port(), "/dc-multi", {{"Accept-Encoding", "gzip"}});
    EXPECT_EQ(resp.statusCode, http::StatusCodeOK) << "request " << ii;
    auto it = resp.headers.find(http::ContentEncoding.get());
    ASSERT_NE(it, resp.headers.end()) << "request " << ii;
    EXPECT_EQ(it->second, "gzip") << "request " << ii;

    auto decompressed = test::Decompress(Encoding::gzip, resp.body);
    EXPECT_EQ(std::string_view(decompressed.data(), decompressed.size()), payload) << "request " << ii;
  }
}

TEST(HttpCompression, DirectCompression_MakeResponseWithBodyOverload) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.compression.minBytes = 32;
    cfg.compression.defaultDirectCompressionMode = DirectCompressionMode::Auto;
    cfg.compression.preferredFormats = {Encoding::gzip};
  });

  std::string payload(256, 'O');
  ts.router().setDefault([&payload](const HttpRequestView& req) {
    // Use the overload that sets body directly
    return req.makeResponse(std::string_view{payload}, "text/plain");
  });

  auto resp = test::simpleGet(ts.port(), "/dc-overload", {{"Accept-Encoding", "gzip"}});
  EXPECT_EQ(resp.statusCode, http::StatusCodeOK);
  auto it = resp.headers.find(http::ContentEncoding.get());
  ASSERT_NE(it, resp.headers.end());
  EXPECT_EQ(it->second, "gzip");

  auto decompressed = test::Decompress(Encoding::gzip, resp.body);
  EXPECT_EQ(std::string_view(decompressed.data(), decompressed.size()), payload);
}

TEST(HttpCompression, DirectCompression_ConfigDefaultModeOn) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.compression.minBytes = 4096;                                           // high threshold
    cfg.compression.defaultDirectCompressionMode = DirectCompressionMode::On;  // bypasses minBytes
    cfg.compression.preferredFormats = {Encoding::gzip};
  });

  std::string payload(128, 'C');  // below minBytes
  ts.router().setDefault([&payload](const HttpRequestView& req) {
    auto resp = req.makeResponse();
    resp.body(std::string_view{payload}, "text/plain");
    return resp;
  });

  auto resp = test::simpleGet(ts.port(), "/dc-cfg-on", {{"Accept-Encoding", "gzip"}});
  EXPECT_EQ(resp.statusCode, http::StatusCodeOK);
  auto it = resp.headers.find(http::ContentEncoding.get());
  ASSERT_NE(it, resp.headers.end());
  EXPECT_EQ(it->second, "gzip");
  EXPECT_LT(resp.body.size(), payload.size());

  auto decompressed = test::Decompress(Encoding::gzip, resp.body);
  EXPECT_EQ(std::string_view(decompressed.data(), decompressed.size()), payload);

  // Reset to Auto for subsequent tests
  ts.postConfigUpdate(
      [](HttpServerConfig& cfg) { cfg.compression.defaultDirectCompressionMode = DirectCompressionMode::Auto; });
}

TEST(HttpCompression, DirectCompression_CustomHeadersPreserved) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.compression.minBytes = 32;
    cfg.compression.defaultDirectCompressionMode = DirectCompressionMode::Auto;
    cfg.compression.preferredFormats = {Encoding::gzip};
  });

  std::string payload(256, 'H');
  ts.router().setDefault([&payload](const HttpRequestView& req) {
    auto resp = req.makeResponse();
    resp.header("x-custom-one", "value1");
    resp.header("x-custom-two", "value2");
    resp.body(std::string_view{payload}, "text/plain");
    return resp;
  });

  auto resp = test::simpleGet(ts.port(), "/dc-headers", {{"Accept-Encoding", "gzip"}});
  EXPECT_EQ(resp.statusCode, http::StatusCodeOK);
  auto it = resp.headers.find(http::ContentEncoding.get());
  ASSERT_NE(it, resp.headers.end());
  EXPECT_EQ(it->second, "gzip");

  // Custom headers must survive
  auto itC1 = resp.headers.find("x-custom-one");
  ASSERT_NE(itC1, resp.headers.end());
  EXPECT_EQ(itC1->second, "value1");
  auto itC2 = resp.headers.find("x-custom-two");
  ASSERT_NE(itC2, resp.headers.end());
  EXPECT_EQ(itC2->second, "value2");
}

#endif  // AERONET_ENABLE_ZLIB

#ifdef AERONET_ENABLE_BROTLI

TEST(HttpCompression, DirectCompression_BrotliRoundTrip) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.compression.minBytes = 32;
    cfg.compression.defaultDirectCompressionMode = DirectCompressionMode::Auto;
    cfg.compression.preferredFormats = {Encoding::br};
  });

  std::string payload(512, 'R');
  ts.router().setDefault([&payload](const HttpRequestView& req) {
    auto resp = req.makeResponse();
    resp.body(std::string_view{payload});
    return resp;
  });

  auto resp = test::simpleGet(ts.port(), "/dc-br", {{"Accept-Encoding", "br"}});
  EXPECT_EQ(resp.statusCode, http::StatusCodeOK);
  auto it = resp.headers.find(http::ContentEncoding.get());
  ASSERT_NE(it, resp.headers.end());
  EXPECT_EQ(it->second, "br");
  EXPECT_LT(resp.body.size(), payload.size());

  auto decompressed = test::Decompress(Encoding::br, resp.body);
  EXPECT_EQ(std::string_view(decompressed.data(), decompressed.size()), payload);
}

#endif  // AERONET_ENABLE_BROTLI

#ifdef AERONET_ENABLE_ZSTD

TEST(HttpCompression, DirectCompression_ZstdRoundTrip) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.compression.minBytes = 32;
    cfg.compression.defaultDirectCompressionMode = DirectCompressionMode::Auto;
    cfg.compression.preferredFormats = {Encoding::zstd};
  });

  std::string payload(512, 'Z');
  ts.router().setDefault([&payload](const HttpRequestView& req) {
    auto resp = req.makeResponse();
    resp.body(std::string_view{payload}, "text/plain");
    return resp;
  });

  auto resp = test::simpleGet(ts.port(), "/dc-zstd", {{"Accept-Encoding", "zstd"}});
  EXPECT_EQ(resp.statusCode, http::StatusCodeOK);
  auto it = resp.headers.find(http::ContentEncoding.get());
  ASSERT_NE(it, resp.headers.end());
  EXPECT_EQ(it->second, "zstd");
  EXPECT_TRUE(test::HasZstdMagic(resp.body));
  EXPECT_LT(resp.body.size(), payload.size());

  std::string decompressed = test::ZstdRoundTripDecompress(resp.body, payload.size());
  EXPECT_EQ(decompressed, payload);
}

#endif  // AERONET_ENABLE_ZSTD

// =============================================================================
// HEAD responses carry the headers of the GET ones (RFC 9110 §9.3.2), compression included: their Content-Length is
// the length of the compressed body (RFC 9110 §8.6).
// =============================================================================

#ifdef AERONET_ENABLE_ZLIB

namespace {

constexpr std::size_t kHeadMinBytes = 64;

void ConfigureHeadCompression(bool useLeadingZeroes = true) {
  ts.postConfigUpdate([useLeadingZeroes](HttpServerConfig& cfg) {
    cfg.compression.minBytes = kHeadMinBytes;
    cfg.compression.maxCompressRatio = 0.6F;
    cfg.compression.addVaryAcceptEncodingHeader = true;
    cfg.compression.useLeadingZeroesInContentLength = useLeadingZeroes;
    cfg.compression.defaultDirectCompressionMode = DirectCompressionMode::Auto;
    cfg.compression.preferredFormats = {Encoding::gzip};
    cfg.compression.contentTypeAllowList.clear();
  });
}

// Sends a 'method' request to 'target' with the given Accept-Encoding (none if empty) and HTTP version, and returns the
// parsed response, read until the server closes the connection.
test::ParsedResponse Fetch(std::string_view method, std::string_view target, std::string_view acceptEncoding,
                           std::string_view version = "HTTP/1.1") {
  std::string req = std::format("{} {} {}\r\nhost: x\r\nconnection: close\r\n", method, target, version);
  if (!acceptEncoding.empty()) {
    req += std::format("accept-encoding: {}\r\n", acceptEncoding);
  }
  req += http::CRLF;
  test::ClientConnection cnx(ts.port());
  test::sendAll(cnx.fd(), req);
  return test::parseResponseOrThrow(test::recvUntilClosed(cnx.fd()));
}

// Checks that the Content-Length of 'resp' is 'expected', compared as a number, and without leading zeroes unless
// 'leadingZeroesAllowed'.
void ExpectContentLength(const test::ParsedResponse& resp, std::size_t expected, bool leadingZeroesAllowed) {
  const std::string value = test::getHeader(resp, http::ContentLength);
  ASSERT_FALSE(value.empty()) << resp.headersRaw;
  EXPECT_EQ(std::stoull(value), expected) << resp.headersRaw;
  if (!leadingZeroesAllowed) {
    EXPECT_TRUE(value == "0" || !value.starts_with('0')) << resp.headersRaw;
  }
}

// Checks that the HEAD response to 'target' has the status and the headers describing the content of the GET one,
// whose Content-Length is the length of the GET content (de-chunked), and no body. Returns the GET response.
test::ParsedResponse ExpectHeadMatchesGet(std::string_view target, std::string_view acceptEncoding,
                                          std::string_view version, bool useLeadingZeroes) {
  test::ParsedResponse get = Fetch("GET", target, acceptEncoding, version);
  const test::ParsedResponse head = Fetch("HEAD", target, acceptEncoding, version);
  EXPECT_EQ(head.statusCode, get.statusCode);
  for (std::string_view name : {
           http::ContentType.get(),
           http::ContentEncoding.get(),
           http::Vary.get(),
           http::Trailer.get(),
           std::string_view{"x-custom"},
       }) {
    EXPECT_EQ(test::getHeader(head, name), test::getHeader(get, name)) << name << '\n' << head.headersRaw;
  }
  if (get.chunked) {
    // Sent chunked for its trailers: the HEAD response announces the length of the de-chunked content.
    ExpectContentLength(head, get.body.size(), useLeadingZeroes);
  } else {
    ExpectContentLength(get, get.body.size(), useLeadingZeroes);
    EXPECT_EQ(test::getHeader(head, http::ContentLength), test::getHeader(get, http::ContentLength));
  }
  EXPECT_FALSE(head.headers.contains(http::TransferEncoding.get())) << head.headersRaw;
  EXPECT_TRUE(head.body.empty()) << head.body.size();
  return get;
}

// How the body of a GET response is compressed when a coding is negotiated.
enum class GetCompression : std::uint8_t {
  None,       // too small, or does not shrink enough
  Direct,     // while the body is set (makeResponse(), then body() / bodyAppend()), with an exact Content-Length
  Finalized,  // when the response is finalized, Content-Length keeping the width of the largest accepted size
};

}  // namespace

TEST(HttpCompression, HeadCarriesTheHeadersOfTheGet) {
  const std::string payload = test::MakePatternedPayload(4000);
  const std::string largePayload = test::MakePatternedPayload(1UL << 20U);
  const RawChars randomChars = test::MakeRandomPayload(64UL << 10U);
  const std::string random(randomChars.data(), randomChars.size());
  static constexpr auto kGetHead = http::Method::GET | http::Method::HEAD;

  auto router = ts.router();
  router.setPath(kGetHead, "/head-gz/plain",
                 [payload](const HttpRequestView&) { return HttpResponse(payload).headerAddLine("x-custom", "kept"); });
  router.setPath(kGetHead, "/head-gz/prepared-inline",
                 [payload](const HttpRequestView& req) { return req.makeResponse(std::string_view(payload)); });
  router.setPath(kGetHead, "/head-gz/captured",
                 [payload](const HttpRequestView& req) { return req.makeResponse().body(std::string(payload)); });
  router.setPath(kGetHead, "/head-gz/captured-large", [largePayload](const HttpRequestView& req) {
    return req.makeResponse().body(std::string(largePayload));
  });
  router.setPath(kGetHead, "/head-gz/direct", [payload](const HttpRequestView& req) {
    auto resp = req.makeResponse();
    resp.body(std::string_view(payload));
    return resp;
  });
  router.setPath(kGetHead, "/head-gz/direct-append", [payload](const HttpRequestView& req) {
    const std::string_view view(payload);
    auto resp = req.makeResponse();
    resp.body(view.substr(0, view.size() / 2));
    resp.bodyAppend(view.substr(view.size() / 2));
    return resp;
  });
  router.setPath(kGetHead, "/head-gz/direct-large", [largePayload](const HttpRequestView& req) {
    auto resp = req.makeResponse();
    resp.body(std::string_view(largePayload));
    return resp;
  });
  router.setPath(kGetHead, "/head-gz/random-plain", [random](const HttpRequestView&) {
    return HttpResponse(random, http::ContentTypeApplicationOctetStream);
  });
  router.setPath(kGetHead, "/head-gz/random-captured", [random](const HttpRequestView& req) {
    return req.makeResponse().body(std::string(random), http::ContentTypeApplicationOctetStream);
  });
  router.setPath(kGetHead, "/head-gz/small", [](const HttpRequestView& req) { return req.makeResponse("tiny"); });
  // The body of a 404 without body is the default page, whose headers a HEAD response announces too. It does not
  // shrink enough to be compressed.
  router.setPath(kGetHead, "/head-gz/not-found",
                 [](const HttpRequestView&) { return HttpResponse(http::StatusCodeNotFound); });
  // A GET response with trailers is chunked (over HTTP/1.1): the HEAD one announces the length of its content.
  router.setPath(kGetHead, "/head-gz/plain-trailers", [payload](const HttpRequestView&) {
    return HttpResponse(payload).trailerAddLine("x-checksum", "abc");
  });
  router.setPath(kGetHead, "/head-gz/direct-trailers", [payload](const HttpRequestView& req) {
    auto resp = req.makeResponse();
    resp.body(std::string_view(payload));
    resp.trailerAddLine("x-checksum", "abc");
    return resp;
  });

  struct HeadCase {
    std::string_view target;
    GetCompression compression;
  };
  static constexpr HeadCase kCases[]{
      {"/head-gz/plain", GetCompression::Finalized},        {"/head-gz/prepared-inline", GetCompression::Finalized},
      {"/head-gz/captured", GetCompression::Finalized},     {"/head-gz/captured-large", GetCompression::Finalized},
      {"/head-gz/direct", GetCompression::Direct},          {"/head-gz/direct-append", GetCompression::Direct},
      {"/head-gz/direct-large", GetCompression::Direct},    {"/head-gz/random-plain", GetCompression::None},
      {"/head-gz/random-captured", GetCompression::None},   {"/head-gz/small", GetCompression::None},
      {"/head-gz/not-found", GetCompression::None},         {"/head-gz/plain-trailers", GetCompression::Finalized},
      {"/head-gz/direct-trailers", GetCompression::Direct},
  };
  for (bool useLeadingZeroes : {true, false}) {
    ConfigureHeadCompression(useLeadingZeroes);
    for (const HeadCase& headCase : kCases) {
      for (std::string_view version : {"HTTP/1.1", "HTTP/1.0"}) {
        for (std::string_view acceptEncoding : {"gzip", ""}) {
          SCOPED_TRACE(std::format("{} {} accept-encoding '{}' leading zeroes {}", headCase.target, version,
                                   acceptEncoding, useLeadingZeroes));
          const test::ParsedResponse get =
              ExpectHeadMatchesGet(headCase.target, acceptEncoding, version, useLeadingZeroes);
          const bool compressed = headCase.compression != GetCompression::None && !acceptEncoding.empty();
          EXPECT_EQ(test::getHeader(get, http::ContentEncoding), compressed ? "gzip" : "") << get.headersRaw;
          if (!get.chunked) {
            // The compressed sizes have fewer digits than the largest accepted ones (60% of the body size).
            const bool padded = compressed && headCase.compression == GetCompression::Finalized && useLeadingZeroes;
            EXPECT_EQ(test::getHeader(get, http::ContentLength).starts_with('0'), padded) << get.headersRaw;
          }
        }
      }
    }
  }
}

// The Content-Length written by the compression of a body may be padded with leading zeros to the number of digits of
// the largest accepted compressed size: the chunked framing that replaces it for trailers must account for its real
// size.
TEST(HttpCompression, CompressedBodyWithTrailersKeepsItsChunkedFraming) {
  const std::string payload = test::MakePatternedPayload(4000);  // compresses to far less than 2400 bytes (4 digits)
  static constexpr auto kGetHead = http::Method::GET | http::Method::HEAD;
  ts.router().setPath(kGetHead, "/trailers-gz/inline", [payload](const HttpRequestView&) {
    return HttpResponse(std::string_view(payload)).trailerAddLine("x-checksum", "abc");
  });
  ts.router().setPath(kGetHead, "/trailers-gz/captured", [payload](const HttpRequestView&) {
    return HttpResponse(std::string(payload)).trailerAddLine("x-checksum", "abc");
  });
  ts.router().setPath(kGetHead, "/trailers-gz/prepared", [payload](const HttpRequestView& req) {
    return req.makeResponse(std::string_view(payload)).trailerAddLine("x-checksum", "abc");
  });

  for (bool useLeadingZeroes : {true, false}) {
    ConfigureHeadCompression(useLeadingZeroes);
    for (std::string_view target : {"/trailers-gz/inline", "/trailers-gz/captured", "/trailers-gz/prepared"}) {
      SCOPED_TRACE(std::format("{} leading zeroes {}", target, useLeadingZeroes));
      test::ClientConnection cnx(ts.port());
      test::sendAll(
          cnx.fd(),
          std::format("GET {} HTTP/1.1\r\nhost: x\r\nconnection: close\r\naccept-encoding: gzip\r\n\r\n", target));
      const std::string raw = test::recvUntilClosed(cnx.fd());
      const test::ParsedResponse resp = test::parseResponseOrThrow(raw);
      ASSERT_TRUE(resp.chunked) << resp.headersRaw;
      EXPECT_EQ(test::getHeader(resp, http::ContentEncoding), "gzip");
      EXPECT_FALSE(resp.headers.contains(http::ContentLength.get())) << resp.headersRaw;
      // The first chunk size follows the head, its body is the whole compressed content, then the trailers.
      EXPECT_EQ(
          raw.substr(resp.headersRaw.size(), raw.find(http::CRLF, resp.headersRaw.size()) - resp.headersRaw.size()),
          std::format("{:x}", resp.body.size()));
      EXPECT_TRUE(raw.ends_with("\r\n0\r\nx-checksum: abc\r\n\r\n")) << raw.substr(raw.size() - 32);
      const RawChars decompressed = test::Decompress(Encoding::gzip, resp.body);
      EXPECT_EQ(std::string_view(decompressed.data(), decompressed.size()), payload);
    }
  }
}

TEST(HttpCompression, StreamingHeadCarriesTheHeadersOfTheGetWithoutContentLength) {
  ConfigureHeadCompression();

  static constexpr auto kGetHead = http::Method::GET | http::Method::HEAD;
  const std::string payload = test::MakePatternedPayload(4000);
  ts.router().setPath(kGetHead, "/head-gz/streamed", [payload](const HttpRequestView&, HttpResponseWriter& writer) {
    const std::string_view view(payload);
    writer.contentType(http::ContentTypeTextPlain);
    writer.writeBody(view.substr(0, kHeadMinBytes / 2));  // buffered, below the compression threshold
    writer.writeBody(view.substr(kHeadMinBytes / 2));     // compression starts
    writer.writeBody(view);                               // compressed
    writer.end();
  });
  ts.router().setPath(kGetHead, "/head-gz/streamed-small", [](const HttpRequestView&, HttpResponseWriter& writer) {
    writer.writeBody("tiny");
    writer.end();
  });

  for (std::string_view target : {"/head-gz/streamed", "/head-gz/streamed-small"}) {
    SCOPED_TRACE(target);
    const test::ParsedResponse get = Fetch("GET", target, "gzip");
    const test::ParsedResponse head = Fetch("HEAD", target, "gzip");
    ASSERT_EQ(get.statusCode, http::StatusCodeOK);
    ASSERT_EQ(head.statusCode, http::StatusCodeOK);
    EXPECT_TRUE(get.chunked) << get.headersRaw;
    for (std::string_view name : {http::ContentType.get(), http::ContentEncoding.get(), http::Vary.get()}) {
      EXPECT_EQ(test::getHeader(head, name), test::getHeader(get, name)) << name << '\n' << head.headersRaw;
    }
    // The length of the chunked GET content is not known when the head is sent.
    EXPECT_FALSE(head.headers.contains(http::ContentLength.get())) << head.headersRaw;
    EXPECT_FALSE(head.headers.contains(http::TransferEncoding.get())) << head.headersRaw;
    EXPECT_TRUE(head.body.empty()) << head.body.size();
  }

  const test::ParsedResponse get = Fetch("GET", "/head-gz/streamed", "gzip");
  EXPECT_EQ(test::getHeader(get, http::ContentEncoding), "gzip");
  const RawChars decompressed = test::Decompress(Encoding::gzip, get.body);
  EXPECT_EQ(std::string_view(decompressed.data(), decompressed.size()), payload + payload);
}

#endif  // AERONET_ENABLE_ZLIB
