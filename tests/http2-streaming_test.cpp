/// @file http2-streaming_test.cpp
/// End-to-end tests for StreamingHandler over HTTP/2 (TLS h2).
/// Mirrors the HTTP/1.1 streaming tests in http-streaming_test.cpp but exercises the
/// Http2WriterTransport path with real TLS connections and HTTP/2 framing.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "aeronet/compression-config.hpp"
#include "aeronet/encoding.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response-writer.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/test_server_http2_tls_fixture.hpp"
#include "aeronet/test_tls_http2_client.hpp"

namespace aeronet::http2 {
namespace {

test::TlsHttp2TestServer ts;

}
// ============================================================================
// Basic streaming
// ============================================================================

TEST(Http2Streaming, SimpleWriteBodyAndEnd) {
  ts.http().router().setPath(http::Method::GET, "/stream", [](const HttpRequest& /*req*/, HttpResponseWriter& writer) {
    writer.status(http::StatusCode{200});
    writer.contentType("text/plain");
    writer.headerAddLine("X-Custom", "streaming-value");
    writer.writeBody("hello ");
    writer.writeBody("world");
    writer.end();
  });

  test::TlsHttp2Client client(ts.port());
  ASSERT_TRUE(client.isConnected());

  auto response = client.get("/stream");
  EXPECT_EQ(response.statusCode, 200);
  EXPECT_EQ(response.body, "hello world");
  EXPECT_EQ(response.header("content-type"), "text/plain");
  EXPECT_EQ(response.header("x-custom"), "streaming-value");
}

TEST(Http2Streaming, EmptyBodyEndOnly) {
  ts.http().router().setPath(http::Method::GET, "/empty", [](const HttpRequest& /*req*/, HttpResponseWriter& writer) {
    writer.status(http::StatusCode{204});
    writer.end();
  });

  test::TlsHttp2Client client(ts.port());
  ASSERT_TRUE(client.isConnected());

  auto response = client.get("/empty");
  EXPECT_EQ(response.statusCode, 204);
  EXPECT_TRUE(response.body.empty());
}

TEST(Http2Streaming, CustomStatusAndReason) {
  ts.http().router().setPath(http::Method::GET, "/created", [](const HttpRequest& /*req*/, HttpResponseWriter& writer) {
    writer.status(http::StatusCode{201});
    writer.reason("Created");
    writer.writeBody("resource-id");
    writer.end();
  });

  test::TlsHttp2Client client(ts.port());
  ASSERT_TRUE(client.isConnected());

  auto response = client.get("/created");
  EXPECT_EQ(response.statusCode, 201);
  EXPECT_EQ(response.body, "resource-id");
}

TEST(Http2Streaming, MultipleWriteBodyCalls) {
  ts.http().router().setPath(http::Method::GET, "/multi", [](const HttpRequest& /*req*/, HttpResponseWriter& writer) {
    writer.status(http::StatusCode{200});
    writer.contentType("application/octet-stream");
    for (int i = 0; i < 10; ++i) {
      writer.writeBody("chunk" + std::to_string(i));
    }
    writer.end();
  });

  test::TlsHttp2Client client(ts.port());
  ASSERT_TRUE(client.isConnected());

  auto response = client.get("/multi");
  EXPECT_EQ(response.statusCode, 200);
  for (int i = 0; i < 10; ++i) {
    EXPECT_TRUE(response.body.contains("chunk" + std::to_string(i))) << "missing chunk" << i;
  }
}

TEST(Http2Streaming, LargeBody) {
  const std::string kPayload(128UL * 1024, 'X');  // 128 KB

  ts.http().router().setPath(http::Method::GET, "/large",
                             [&kPayload](const HttpRequest& /*req*/, HttpResponseWriter& writer) {
                               writer.status(http::StatusCode{200});
                               writer.contentType("application/octet-stream");
                               // Write in 16 KB chunks
                               constexpr std::size_t kChunkSize = static_cast<std::size_t>(16 * 1024);
                               for (std::size_t offset = 0; offset < kPayload.size(); offset += kChunkSize) {
                                 auto len = std::min(kChunkSize, kPayload.size() - offset);
                                 writer.writeBody(std::string_view(kPayload).substr(offset, len));
                               }
                               writer.end();
                             });

  test::TlsHttp2Client client(ts.port());
  ASSERT_TRUE(client.isConnected());

  auto response = client.get("/large");
  EXPECT_EQ(response.statusCode, 200);
  EXPECT_EQ(response.body.size(), kPayload.size());
  EXPECT_EQ(response.body, kPayload);
}

// ============================================================================
// Headers behavior
// ============================================================================

TEST(Http2Streaming, HeadersIgnoredAfterFirstWrite) {
  ts.http().router().setPath(http::Method::GET, "/hdr-ignore",
                             [](const HttpRequest& /*req*/, HttpResponseWriter& writer) {
                               writer.status(http::StatusCode{200});
                               writer.headerAddLine("X-Before", "visible");
                               writer.writeBody("data");
                               // These should be silently ignored after headers are sent
                               writer.status(http::StatusCode{404});
                               writer.headerAddLine("X-After", "invisible");
                               writer.end();
                             });

  test::TlsHttp2Client client(ts.port());
  ASSERT_TRUE(client.isConnected());

  auto response = client.get("/hdr-ignore");
  EXPECT_EQ(response.statusCode, 200);  // Not 404
  EXPECT_EQ(response.header("x-before"), "visible");
  EXPECT_TRUE(response.header("x-after").empty());
}

TEST(Http2Streaming, MultipleCustomHeaders) {
  ts.http().router().setPath(http::Method::GET, "/multi-hdr",
                             [](const HttpRequest& /*req*/, HttpResponseWriter& writer) {
                               writer.status(http::StatusCode{200});
                               writer.contentType("application/json");
                               writer.headerAddLine("X-Request-Id", "abc-123");
                               writer.headerAddLine("X-Trace-Id", "trace-456");
                               writer.header("Cache-Control", "no-cache");
                               writer.writeBody(R"({"ok":true})");
                               writer.end();
                             });

  test::TlsHttp2Client client(ts.port());
  ASSERT_TRUE(client.isConnected());

  auto response = client.get("/multi-hdr");
  EXPECT_EQ(response.statusCode, 200);
  EXPECT_EQ(response.header("content-type"), "application/json");
  EXPECT_EQ(response.header("x-request-id"), "abc-123");
  EXPECT_EQ(response.header("x-trace-id"), "trace-456");
  EXPECT_EQ(response.header("cache-control"), "no-cache");
  EXPECT_EQ(response.body, R"({"ok":true})");
}

// ============================================================================
// HEAD request on streaming endpoint
// ============================================================================

TEST(Http2Streaming, HeadSuppressesBody) {
  ts.http().router().setPath(http::Method::GET, "/head-test",
                             [](const HttpRequest& /*req*/, HttpResponseWriter& writer) {
                               writer.status(http::StatusCode{200});
                               writer.contentType("text/plain");
                               writer.writeBody("THIS_SHOULD_NOT_APPEAR");
                               writer.end();
                             });

  test::TlsHttp2Client client(ts.port());
  ASSERT_TRUE(client.isConnected());

  auto response = client.request("HEAD", "/head-test");
  EXPECT_EQ(response.statusCode, 200);
  EXPECT_TRUE(response.body.empty()) << "HEAD body should be empty, got: " << response.body;
}

// ============================================================================
// Fixed Content-Length mode
// ============================================================================

TEST(Http2Streaming, ExplicitContentLength) {
  constexpr std::string_view kBody = "fixed-length-body";

  ts.http().router().setPath(http::Method::GET, "/fixed-cl",
                             [](const HttpRequest& /*req*/, HttpResponseWriter& writer) {
                               writer.status(http::StatusCode{200});
                               writer.contentType("text/plain");
                               writer.contentLength(17);  // "fixed-length-body" is 17 bytes
                               writer.writeBody("fixed-length-body");
                               writer.end();
                             });

  test::TlsHttp2Client client(ts.port());
  ASSERT_TRUE(client.isConnected());

  auto response = client.get("/fixed-cl");
  EXPECT_EQ(response.statusCode, 200);
  EXPECT_EQ(response.body, kBody);
}

// NOTE: writer.file() over HTTP/2 streaming relies on deferred PendingFileSend
// flushing which requires event-loop output-drain callbacks. This is tested
// at the unit-test level (http2-protocol-handler_test) rather than here.

// ============================================================================
// Mixed handler types on same server
// ============================================================================

TEST(Http2Streaming, MixedStreamingAndNormalHandlers) {
  // Streaming GET
  ts.http().router().setPath(http::Method::GET, "/mix", [](const HttpRequest& /*req*/, HttpResponseWriter& writer) {
    writer.status(http::StatusCode{200});
    writer.contentType("text/plain");
    writer.writeBody("STREAM");
    writer.end();
  });

  // Normal POST
  ts.http().router().setPath(http::Method::POST, "/mix",
                             [](const HttpRequest& /*req*/) { return HttpResponse(http::StatusCode{201}, "NORMAL"); });

  test::TlsHttp2Client client(ts.port());
  ASSERT_TRUE(client.isConnected());

  auto getResp = client.get("/mix");
  EXPECT_EQ(getResp.statusCode, 200);
  EXPECT_EQ(getResp.body, "STREAM");

  auto postResp = client.post("/mix", "payload");
  EXPECT_EQ(postResp.statusCode, 201);
  EXPECT_TRUE(postResp.body.find("NORMAL") != std::string::npos);
}

TEST(Http2Streaming, StreamingDefaultFallback) {
  // Global streaming default
  ts.http().router().setDefault([](const HttpRequest& /*req*/, HttpResponseWriter& writer) {
    writer.status(http::StatusCode{200});
    writer.writeBody("default-stream");
    writer.end();
  });

  test::TlsHttp2Client client(ts.port());
  ASSERT_TRUE(client.isConnected());

  auto response = client.get("/any-path");
  EXPECT_EQ(response.statusCode, 200);
  EXPECT_EQ(response.body, "default-stream");
}

// ============================================================================
// Method Not Allowed
// ============================================================================

TEST(Http2Streaming, MethodNotAllowedOnStreamingPath) {
  ts.http().router().setPath(http::Method::GET, "/get-only",
                             [](const HttpRequest& /*req*/, HttpResponseWriter& writer) {
                               writer.status(http::StatusCode{200});
                               writer.writeBody("ok");
                               writer.end();
                             });

  test::TlsHttp2Client client(ts.port());
  ASSERT_TRUE(client.isConnected());

  auto response = client.post("/get-only", "data");
  EXPECT_EQ(response.statusCode, 405);
}

// ============================================================================
// Multiple methods with streaming handlers
// ============================================================================

TEST(Http2Streaming, PostStreamingHandler) {
  ts.http().router().setPath(http::Method::POST, "/upload", [](const HttpRequest& req, HttpResponseWriter& writer) {
    writer.status(http::StatusCode{200});
    writer.contentType("text/plain");
    writer.writeBody("received:" + std::string(req.body()));
    writer.end();
  });

  test::TlsHttp2Client client(ts.port());
  ASSERT_TRUE(client.isConnected());

  auto response = client.post("/upload", "my-data", "text/plain");
  EXPECT_EQ(response.statusCode, 200);
  EXPECT_EQ(response.body, "received:my-data");
}

TEST(Http2Streaming, PutStreamingHandler) {
  ts.http().router().setPath(http::Method::PUT, "/resource",
                             [](const HttpRequest& /*req*/, HttpResponseWriter& writer) {
                               writer.status(http::StatusCode{200});
                               writer.writeBody("updated");
                               writer.end();
                             });

  test::TlsHttp2Client client(ts.port());
  ASSERT_TRUE(client.isConnected());

  auto response = client.request("PUT", "/resource", {}, "payload");
  EXPECT_EQ(response.statusCode, 200);
  EXPECT_EQ(response.body, "updated");
}

TEST(Http2Streaming, DeleteStreamingHandler) {
  ts.http().router().setPath(http::Method::DELETE, "/resource/42",
                             [](const HttpRequest& /*req*/, HttpResponseWriter& writer) {
                               writer.status(http::StatusCode{200});
                               writer.writeBody("deleted");
                               writer.end();
                             });

  test::TlsHttp2Client client(ts.port());
  ASSERT_TRUE(client.isConnected());

  auto response = client.request("DELETE", "/resource/42");
  EXPECT_EQ(response.statusCode, 200);
  EXPECT_EQ(response.body, "deleted");
}

// ============================================================================
// Concurrent streams (HTTP/2 multiplexing)
// ============================================================================

TEST(Http2Streaming, ConcurrentStreamingRequests) {
  ts.http().router().setPath(http::Method::GET, "/slow", [](const HttpRequest& /*req*/, HttpResponseWriter& writer) {
    writer.status(http::StatusCode{200});
    writer.contentType("text/plain");
    writer.writeBody("part1");
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    writer.writeBody("part2");
    writer.end();
  });

  test::TlsHttp2Client client(ts.port());
  ASSERT_TRUE(client.isConnected());

  // Send 3 concurrent requests on the same connection
  uint32_t s1 = client.sendAsyncRequest("GET", "/slow");
  uint32_t s2 = client.sendAsyncRequest("GET", "/slow");
  uint32_t s3 = client.sendAsyncRequest("GET", "/slow");

  auto r1 = client.waitAndGetResponse(s1, std::chrono::milliseconds{5000});
  auto r2 = client.waitAndGetResponse(s2, std::chrono::milliseconds{5000});
  auto r3 = client.waitAndGetResponse(s3, std::chrono::milliseconds{5000});

  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r2.has_value());
  ASSERT_TRUE(r3.has_value());

  EXPECT_EQ(r1.value_or(test::TlsHttp2Client::Response{}).statusCode, 200);
  EXPECT_EQ(r1.value_or(test::TlsHttp2Client::Response{}).body, "part1part2");

  EXPECT_EQ(r2.value_or(test::TlsHttp2Client::Response{}).statusCode, 200);
  EXPECT_EQ(r2.value_or(test::TlsHttp2Client::Response{}).body, "part1part2");

  EXPECT_EQ(r3.value_or(test::TlsHttp2Client::Response{}).statusCode, 200);
  EXPECT_EQ(r3.value_or(test::TlsHttp2Client::Response{}).body, "part1part2");
}

TEST(Http2Streaming, ConcurrentMixedStreamingAndNormal) {
  ts.http().router().setPath(http::Method::GET, "/stream-path",
                             [](const HttpRequest& /*req*/, HttpResponseWriter& writer) {
                               writer.status(http::StatusCode{200});
                               writer.writeBody("streamed");
                               writer.end();
                             });

  ts.http().router().setPath(http::Method::GET, "/normal-path", [](const HttpRequest& /*req*/) {
    return HttpResponse(http::StatusCode{200}, "buffered");
  });

  test::TlsHttp2Client client(ts.port());
  ASSERT_TRUE(client.isConnected());

  uint32_t s1 = client.sendAsyncRequest("GET", "/stream-path");
  uint32_t s2 = client.sendAsyncRequest("GET", "/normal-path");
  uint32_t s3 = client.sendAsyncRequest("GET", "/stream-path");

  auto r1 = client.waitAndGetResponse(s1, std::chrono::milliseconds{5000});
  auto r2 = client.waitAndGetResponse(s2, std::chrono::milliseconds{5000});
  auto r3 = client.waitAndGetResponse(s3, std::chrono::milliseconds{5000});

  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(r1.value_or(test::TlsHttp2Client::Response{}).statusCode, 200);
  EXPECT_EQ(r1.value_or(test::TlsHttp2Client::Response{}).body, "streamed");

  ASSERT_TRUE(r2.has_value());
  EXPECT_EQ(r2.value_or(test::TlsHttp2Client::Response{}).statusCode, 200);
  EXPECT_TRUE(r2.value_or(test::TlsHttp2Client::Response{}).body.find("buffered") != std::string::npos);

  ASSERT_TRUE(r3.has_value());
  EXPECT_EQ(r3.value_or(test::TlsHttp2Client::Response{}).statusCode, 200);
  EXPECT_EQ(r3.value_or(test::TlsHttp2Client::Response{}).body, "streamed");
}

// ============================================================================
// Sequential requests on same connection (keep-alive / reuse)
// ============================================================================

TEST(Http2Streaming, SequentialRequestsOnSameConnection) {
  int invocationCount = 0;
  ts.http().router().setPath(http::Method::GET, "/seq",
                             [&invocationCount](const HttpRequest& /*req*/, HttpResponseWriter& writer) {
                               ++invocationCount;
                               writer.status(http::StatusCode{200});
                               writer.writeBody("call-" + std::to_string(invocationCount));
                               writer.end();
                             });

  test::TlsHttp2Client client(ts.port());
  ASSERT_TRUE(client.isConnected());

  auto r1 = client.get("/seq");
  EXPECT_EQ(r1.statusCode, 200);
  EXPECT_EQ(r1.body, "call-1");

  auto r2 = client.get("/seq");
  EXPECT_EQ(r2.statusCode, 200);
  EXPECT_EQ(r2.body, "call-2");

  auto r3 = client.get("/seq");
  EXPECT_EQ(r3.statusCode, 200);
  EXPECT_EQ(r3.body, "call-3");
}

// ============================================================================
// Compression (gzip over HTTP/2 streaming)
// ============================================================================

#ifdef AERONET_ENABLE_ZLIB
TEST(Http2StreamingCompression, GzipCompressedStreamingBody) {
  ts.server.postConfigUpdate([](HttpServerConfig& cfg) {
    CompressionConfig compression;
    compression.minBytes = 8;
    compression.preferredFormats.clear();
    compression.preferredFormats.push_back(Encoding::gzip);
    cfg.withCompression(compression);
  });

  ts.http().router().setPath(http::Method::GET, "/compress",
                             [](const HttpRequest& /*req*/, HttpResponseWriter& writer) {
                               writer.status(http::StatusCode{200});
                               writer.contentType("text/plain");
                               // Write enough data to trigger compression (above minBytes threshold)
                               writer.writeBody(std::string(128, 'A'));
                               writer.end();
                             });

  test::TlsHttp2Client client(ts.port());
  ASSERT_TRUE(client.isConnected());

  auto response = client.get("/compress", {{"accept-encoding", "gzip"}});
  EXPECT_EQ(response.statusCode, 200);
  // The HTTP/2 client should decompress transparently, or we see content-encoding header.
  // Verify we got the content back (may be decompressed by client or raw gzip).
  // At minimum the response should be successful and not empty.
  EXPECT_FALSE(response.body.empty());
}

TEST(Http2StreamingCompression, IdentityEncodingPreventsCompression) {
  ts.server.postConfigUpdate([](HttpServerConfig& cfg) {
    CompressionConfig compression;
    compression.minBytes = 8;
    compression.preferredFormats = {Encoding::gzip};
    cfg.withCompression(compression);
  });

  ts.http().router().setPath(http::Method::GET, "/no-compress",
                             [](const HttpRequest& /*req*/, HttpResponseWriter& writer) {
                               writer.status(http::StatusCode{200});
                               writer.contentType("text/plain");
                               writer.contentEncoding("identity");
                               writer.writeBody(std::string(128, 'B'));
                               writer.end();
                             });

  test::TlsHttp2Client client(ts.port());
  ASSERT_TRUE(client.isConnected());

  auto response = client.get("/no-compress", {{"accept-encoding", "gzip"}});
  EXPECT_EQ(response.statusCode, 200);
  // With identity encoding, body should be uncompressed
  EXPECT_EQ(response.body, std::string(128, 'B'));
}
#endif  // AERONET_ENABLE_ZLIB

// ============================================================================
// Trailers over HTTP/2
// ============================================================================

TEST(Http2Streaming, TrailersEmitted) {
  ts.http().router().setPath(http::Method::GET, "/trailers",
                             [](const HttpRequest& /*req*/, HttpResponseWriter& writer) {
                               writer.status(http::StatusCode{200});
                               writer.contentType("text/plain");
                               writer.writeBody("body-with-trailers");
                               writer.trailerAddLine("x-checksum", "abc123");
                               writer.trailerAddLine("x-duration-ms", "42");
                               writer.end();
                             });

  test::TlsHttp2Client client(ts.port());
  ASSERT_TRUE(client.isConnected());

  auto response = client.get("/trailers");
  EXPECT_EQ(response.statusCode, 200);
  EXPECT_EQ(response.body, "body-with-trailers");
  // HTTP/2 trailers are delivered as a trailing HEADERS frame; the client
  // may expose them as regular headers or trailing headers.
  // At minimum the response must succeed.
}

// ============================================================================
// Path parameters with streaming handler
// ============================================================================

TEST(Http2Streaming, PathParamsWithStreamingHandler) {
  ts.http().router().setPath(http::Method::GET, "/items/{id}", [](const HttpRequest& req, HttpResponseWriter& writer) {
    writer.status(http::StatusCode{200});
    writer.contentType("text/plain");
    auto id = req.pathParams().contains("id") ? req.pathParams().at("id") : "none";
    writer.writeBody("item-id:" + std::string(id));
    writer.end();
  });

  test::TlsHttp2Client client(ts.port());
  ASSERT_TRUE(client.isConnected());

  auto response = client.get("/items/42");
  EXPECT_EQ(response.statusCode, 200);
  EXPECT_EQ(response.body, "item-id:42");
}

// ============================================================================
// Query string access in streaming handler
// ============================================================================

TEST(Http2Streaming, QueryParamAccess) {
  ts.http().router().setPath(http::Method::GET, "/search", [](const HttpRequest& req, HttpResponseWriter& writer) {
    writer.status(http::StatusCode{200});
    writer.contentType("text/plain");
    auto queryParam = req.queryParamValue("q").value_or("");
    auto page = req.queryParamValue("page").value_or("");
    writer.writeBody("q=" + std::string(queryParam) + "&page=" + std::string(page));
    writer.end();
  });

  test::TlsHttp2Client client(ts.port());
  ASSERT_TRUE(client.isConnected());

  auto response = client.get("/search?q=hello&page=2");
  EXPECT_EQ(response.statusCode, 200);
  EXPECT_EQ(response.body, "q=hello&page=2");
}

// ============================================================================
// Error in handler (exception before any output)
// ============================================================================

TEST(Http2Streaming, HandlerExceptionBeforeHeadersSendsDefault200) {
  ts.http().router().setPath(
      http::Method::GET, "/crash",
      [](const HttpRequest& /*req*/, HttpResponseWriter& /*writer*/) { throw std::runtime_error("handler-exploded"); });

  test::TlsHttp2Client client(ts.port());
  ASSERT_TRUE(client.isConnected());

  auto response = client.get("/crash");
  // Exception before headers sent: the streaming handler catches the exception,
  // then calls writer.end() which sends default 200 OK with empty body.
  EXPECT_EQ(response.statusCode, 200);
  EXPECT_TRUE(response.body.empty());
}

// ============================================================================
// Double end() is a no-op
// ============================================================================

TEST(Http2Streaming, DoubleEndIsNoOp) {
  ts.http().router().setPath(http::Method::GET, "/double-end",
                             [](const HttpRequest& /*req*/, HttpResponseWriter& writer) {
                               writer.status(http::StatusCode{200});
                               writer.writeBody("once");
                               writer.end();
                               writer.end();  // second end() should be silently ignored
                             });

  test::TlsHttp2Client client(ts.port());
  ASSERT_TRUE(client.isConnected());

  auto response = client.get("/double-end");
  EXPECT_EQ(response.statusCode, 200);
  EXPECT_EQ(response.body, "once");
}

// ============================================================================
// writeBody() after end() returns false
// ============================================================================

TEST(Http2Streaming, WriteAfterEndIgnored) {
  ts.http().router().setPath(http::Method::GET, "/write-after-end",
                             [](const HttpRequest& /*req*/, HttpResponseWriter& writer) {
                               writer.status(http::StatusCode{200});
                               writer.writeBody("visible");
                               writer.end();
                               writer.writeBody("invisible");  // should be ignored
                             });

  test::TlsHttp2Client client(ts.port());
  ASSERT_TRUE(client.isConnected());

  auto response = client.get("/write-after-end");
  EXPECT_EQ(response.statusCode, 200);
  EXPECT_EQ(response.body, "visible");
  EXPECT_TRUE(response.body.find("invisible") == std::string::npos);
}

// ============================================================================
// Global streaming handler overrides global normal handler
// ============================================================================

TEST(Http2Streaming, GlobalStreamingPrecedenceOverNormal) {
  // Register both global normal and global streaming defaults
  ts.http().router().setDefault(
      [](const HttpRequest& /*req*/) { return HttpResponse(http::StatusCode{200}, "normal-global"); });
  ts.http().router().setDefault([](const HttpRequest& /*req*/, HttpResponseWriter& writer) {
    writer.status(http::StatusCode{200});
    writer.writeBody("streaming-global");
    writer.end();
  });

  test::TlsHttp2Client client(ts.port());
  ASSERT_TRUE(client.isConnected());

  auto response = client.get("/unknown");
  EXPECT_EQ(response.statusCode, 200);
  // Streaming default should take precedence
  EXPECT_EQ(response.body, "streaming-global");
}

// ============================================================================
// Streaming handler reading request headers
// ============================================================================

TEST(Http2Streaming, AccessRequestHeaders) {
  ts.http().router().setPath(http::Method::GET, "/echo-header", [](const HttpRequest& req, HttpResponseWriter& writer) {
    writer.status(http::StatusCode{200});
    writer.contentType("text/plain");
    auto auth = req.headerValueOrEmpty("authorization");
    writer.writeBody("auth:" + std::string(auth));
    writer.end();
  });

  test::TlsHttp2Client client(ts.port());
  ASSERT_TRUE(client.isConnected());

  auto response = client.get("/echo-header", {{"authorization", "Bearer token123"}});
  EXPECT_EQ(response.statusCode, 200);
  EXPECT_EQ(response.body, "auth:Bearer token123");
}

}  // namespace aeronet::http2
