#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <utility>

#include "aeronet/http-constants.hpp"
#include "aeronet/http-helpers.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response-writer.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/single-http-server.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_util.hpp"

namespace aeronet {

namespace {
test::TestServer ts(HttpServerConfig{});
auto port = ts.port();
}  // namespace

// Basic trailer parsing test
TEST(HttpTrailers, BasicTrailer) {
  ts.router().setDefault([](const HttpRequest& req) {
    EXPECT_EQ(req.body(), "Wikipedia");
    // Check trailer headers
    EXPECT_EQ(req.trailers().size(), 1U);
    auto it = req.trailers().find("X-Checksum");
    EXPECT_NE(it, req.trailers().end());
    if (it != req.trailers().end()) {
      EXPECT_EQ(it->second, "abc123");
    }
    return HttpResponse("OK");
  });

  test::ClientConnection sock(port);
  int fd = sock.fd();

  std::string req =
      "POST /trailer HTTP/1.1\r\n"
      "Host: example.com\r\n"
      "Transfer-Encoding: chunked\r\n"
      "Connection: close\r\n"
      "\r\n"
      "4\r\nWiki\r\n"
      "5\r\npedia\r\n"
      "0\r\n"
      "X-Checksum: abc123\r\n"
      "\r\n";
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 200"));
}

// Multiple trailer headers
TEST(HttpTrailers, MultipleTrailers) {
  ts.router().setDefault([](const HttpRequest& req) {
    EXPECT_EQ(req.body(), "test");
    EXPECT_EQ(req.trailers().size(), 3U);

    auto checksum = req.trailers().find("X-Checksum");
    EXPECT_NE(checksum, req.trailers().end());
    if (checksum != req.trailers().end()) {
      EXPECT_EQ(checksum->second, "xyz789");
    }

    auto timestamp = req.trailers().find("X-Timestamp");
    EXPECT_NE(timestamp, req.trailers().end());
    if (timestamp != req.trailers().end()) {
      EXPECT_EQ(timestamp->second, "2025-10-20T12:00:00Z");
    }

    auto custom = req.trailers().find("X-Custom-Trailer");
    EXPECT_NE(custom, req.trailers().end());
    if (custom != req.trailers().end()) {
      EXPECT_EQ(custom->second, "value123");
    }

    return HttpResponse("OK");
  });

  test::ClientConnection sock(port);
  int fd = sock.fd();

  std::string req =
      "POST /multi HTTP/1.1\r\n"
      "Host: example.com\r\n"
      "Transfer-Encoding: chunked\r\n"
      "Connection: close\r\n"
      "\r\n"
      "4\r\ntest\r\n"
      "0\r\n"
      "X-Checksum: xyz789\r\n"
      "X-Timestamp: 2025-10-20T12:00:00Z\r\n"
      "X-Custom-Trailer: value123\r\n"
      "\r\n";
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.contains("200"));
}

// Empty trailers (just zero chunk and terminating CRLF)
TEST(HttpTrailers, NoTrailers) {
  ts.router().setDefault([](const HttpRequest& req) {
    EXPECT_EQ(req.body(), "data");
    EXPECT_TRUE(req.trailers().empty());
    return HttpResponse("OK");
  });

  test::ClientConnection sock(port);
  int fd = sock.fd();

  std::string req =
      "POST /notrailer HTTP/1.1\r\n"
      "Host: example.com\r\n"
      "Transfer-Encoding: chunked\r\n"
      "Connection: close\r\n"
      "\r\n"
      "4\r\ndata\r\n"
      "0\r\n"
      "\r\n";
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.contains("200"));
}

// Trailer with whitespace trimming
TEST(HttpTrailers, TrailerWhitespaceTrim) {
  ts.router().setDefault([](const HttpRequest& req) {
    auto trailer = req.trailers().find("X-Data");
    EXPECT_NE(trailer, req.trailers().end());
    if (trailer != req.trailers().end()) {
      EXPECT_EQ(trailer->second, "trimmed");  // should be trimmed
    }
    return HttpResponse("OK");
  });

  test::ClientConnection sock(port);
  int fd = sock.fd();

  std::string req =
      "POST /trim HTTP/1.1\r\n"
      "Host: example.com\r\n"
      "Transfer-Encoding: chunked\r\n"
      "Connection: close\r\n"
      "\r\n"
      "2\r\nhi\r\n"
      "0\r\n"
      "X-Data:   trimmed  \r\n"
      "\r\n";
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.contains("200"));
}

// Forbidden trailer: Transfer-Encoding
TEST(HttpTrailers, ForbiddenTrailerTransferEncoding) {
  ts.router().setDefault([](const HttpRequest&) { return HttpResponse("FAIL"); });

  test::ClientConnection sock(port);
  int fd = sock.fd();

  std::string req =
      "POST /forbidden HTTP/1.1\r\n"
      "Host: example.com\r\n"
      "Transfer-Encoding: chunked\r\n"
      "\r\n"
      "4\r\ntest\r\n"
      "0\r\n"
      "Transfer-Encoding: chunked\r\n"
      "\r\n";
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.contains("HTTP/1.1 400"));
}

// Forbidden trailer: Content-Length
TEST(HttpTrailers, ForbiddenTrailerContentLength) {
  ts.router().setDefault([](const HttpRequest&) { return HttpResponse("FAIL"); });

  test::ClientConnection sock(port);
  int fd = sock.fd();

  std::string req =
      "POST /forbidden HTTP/1.1\r\n"
      "Host: example.com\r\n"
      "Transfer-Encoding: chunked\r\n"
      "\r\n"
      "4\r\ntest\r\n"
      "0\r\n"
      "Content-Length: 100\r\n"
      "\r\n";
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.contains("HTTP/1.1 400"));
}

// Forbidden trailer: Host
TEST(HttpTrailers, ForbiddenTrailerHost) {
  ts.router().setDefault([](const HttpRequest&) { return HttpResponse("FAIL"); });

  test::ClientConnection sock(port);
  int fd = sock.fd();

  std::string req =
      "POST /forbidden HTTP/1.1\r\n"
      "Host: example.com\r\n"
      "Transfer-Encoding: chunked\r\n"
      "\r\n"
      "4\r\ntest\r\n"
      "0\r\n"
      "Host: evil.com\r\n"
      "\r\n";
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.contains("HTTP/1.1 400"));
}

// Forbidden trailer: Authorization
TEST(HttpTrailers, ForbiddenTrailerAuthorization) {
  ts.router().setDefault([](const HttpRequest&) { return HttpResponse("FAIL"); });

  test::ClientConnection sock(port);
  int fd = sock.fd();

  std::string req =
      "POST /forbidden HTTP/1.1\r\n"
      "Host: example.com\r\n"
      "Transfer-Encoding: chunked\r\n"
      "\r\n"
      "4\r\ntest\r\n"
      "0\r\n"
      "Authorization: Bearer token123\r\n"
      "\r\n";
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.contains("HTTP/1.1 400"));
}

// Trailer size exceeds limit
TEST(HttpTrailers, TrailerSizeLimit) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.withMaxHeaderBytes(200);  // match header limit
  });
  ts.router().setDefault([](const HttpRequest&) { return HttpResponse("FAIL"); });

  test::ClientConnection sock(port);
  int fd = sock.fd();

  // Create a very large trailer that exceeds the 200-byte limit
  std::string largeValue(300, 'X');
  std::string req =
      "POST /largetrailer HTTP/1.1\r\n"
      "Host: example.com\r\n"
      "Transfer-Encoding: chunked\r\n"
      "\r\n"
      "4\r\ntest\r\n"
      "0\r\n"
      "X-Large: " +
      largeValue +
      "\r\n"
      "\r\n";
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.contains("HTTP/1.1 431"));
}

// Trailer with empty value
TEST(HttpTrailers, TrailerEmptyValue) {
  ts.router().setDefault([](const HttpRequest& req) {
    auto trailer = req.trailers().find("X-Empty");
    EXPECT_NE(trailer, req.trailers().end());
    if (trailer != req.trailers().end()) {
      EXPECT_TRUE(trailer->second.empty());
    }
    return HttpResponse("OK");
  });

  test::ClientConnection sock(port);
  int fd = sock.fd();

  std::string req =
      "POST /empty HTTP/1.1\r\n"
      "Host: example.com\r\n"
      "Transfer-Encoding: chunked\r\n"
      "Connection: close\r\n"
      "\r\n"
      "4\r\ntest\r\n"
      "0\r\n"
      "X-Empty:\r\n"
      "\r\n";
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.contains("200"));
}

// Case-insensitive trailer lookup
TEST(HttpTrailers, TrailerCaseInsensitive) {
  ts.router().setDefault([](const HttpRequest& req) {
    // Should be able to find with different case
    auto lower = req.trailers().find("x-checksum");
    auto upper = req.trailers().find("X-CHECKSUM");
    auto mixed = req.trailers().find("X-Checksum");

    EXPECT_NE(lower, req.trailers().end());
    EXPECT_NE(upper, req.trailers().end());
    EXPECT_NE(mixed, req.trailers().end());

    if (lower != req.trailers().end()) {
      EXPECT_EQ(lower->second, "test123");
    }
    return HttpResponse("OK");
  });

  test::ClientConnection sock(port);
  int fd = sock.fd();

  std::string req =
      "POST /case HTTP/1.1\r\n"
      "Host: example.com\r\n"
      "Transfer-Encoding: chunked\r\n"
      "Connection: close\r\n"
      "\r\n"
      "4\r\ntest\r\n"
      "0\r\n"
      "X-Checksum: test123\r\n"
      "\r\n";
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.contains("200"));
}

// Duplicate trailers that should be merged using list semantics (comma)
TEST(HttpTrailers, DuplicateMergeTrailers) {
  ts.router().setDefault([](const HttpRequest& req) {
    // Accept header should be merged with a comma separator
    auto it = req.trailers().find("Accept");
    EXPECT_NE(it, req.trailers().end());
    if (it != req.trailers().end()) {
      EXPECT_EQ(it->second, "text/html,application/json");
    }
    return HttpResponse("OK");
  });

  test::ClientConnection sock(port);
  int fd = sock.fd();

  std::string req =
      "POST /dupmerge HTTP/1.1\r\n"
      "Host: example.com\r\n"
      "Transfer-Encoding: chunked\r\n"
      "Connection: close\r\n"
      "\r\n"
      "4\r\ntest\r\n"
      "0\r\n"
      "Accept: text/html\r\n"
      "Accept: application/json\r\n"
      "\r\n";
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.contains("200"));
}

// Duplicate trailers with override semantics (keep last)
TEST(HttpTrailers, DuplicateOverrideTrailers) {
  ts.router().setDefault([](const HttpRequest& req) {
    auto it = req.trailers().find("From");
    EXPECT_NE(it, req.trailers().end());
    if (it != req.trailers().end()) {
      // 'From' has override semantics in ReqHeaderValueSeparator, keep the last occurrence
      EXPECT_EQ(it->second, "b@example.com");
    }
    return HttpResponse("OK");
  });

  test::ClientConnection sock(port);
  int fd = sock.fd();

  std::string req =
      "POST /dupoverride HTTP/1.1\r\n"
      "Host: example.com\r\n"
      "Transfer-Encoding: chunked\r\n"
      "Connection: close\r\n"
      "\r\n"
      "4\r\ntest\r\n"
      "0\r\n"
      "From: a@example.com\r\n"
      "From: b@example.com\r\n"
      "\r\n";
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.contains("200"));
}

// Unknown header duplicates when mergeUnknownRequestHeaders is disabled -> should be rejected
TEST(HttpTrailers, UnknownHeaderNoMergeTrailers) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.withMergeUnknownRequestHeaders(false); });

  // "Handler should not be called when unknown-header duplicates are forbidden"
  ts.router().setDefault([](const HttpRequest&) { return HttpResponse("FAIL"); });

  test::ClientConnection sock(port);
  int fd = sock.fd();

  std::string req =
      "POST /unknownnomerge HTTP/1.1\r\n"
      "Host: example.com\r\n"
      "Transfer-Encoding: chunked\r\n"
      "\r\n"
      "4\r\ntest\r\n"
      "0\r\n"
      "X-Experimental: a\r\n"
      "X-Experimental: b\r\n"
      "\r\n";
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.contains("HTTP/1.1 400"));
}

// Malformed trailer (no colon)
TEST(HttpTrailers, MalformedTrailerNoColon) {
  ts.router().setDefault([](const HttpRequest&) { return HttpResponse("FAIL"); });

  test::ClientConnection sock(port);
  int fd = sock.fd();

  std::string req =
      "POST /malformed HTTP/1.1\r\n"
      "Host: example.com\r\n"
      "Transfer-Encoding: chunked\r\n"
      "\r\n"
      "4\r\ntest\r\n"
      "0\r\n"
      "MalformedTrailer\r\n"
      "\r\n";
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.contains("HTTP/1.1 400"));
}

// Non-chunked request should have empty trailers
TEST(HttpTrailers, NonChunkedNoTrailers) {
  ts.router().setDefault([](const HttpRequest& req) {
    EXPECT_EQ(req.body(), "test");
    EXPECT_TRUE(req.trailers().empty());
    return HttpResponse("OK");
  });

  test::ClientConnection sock(port);
  int fd = sock.fd();

  std::string req =
      "POST /fixed HTTP/1.1\r\n"
      "Host: example.com\r\n"
      "Content-Length: 4\r\n"
      "Connection: close\r\n"
      "\r\n"
      "test";
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.contains("200"));
}

// Test streaming response with trailers
TEST(HttpResponseWriterTrailers, BasicStreamingTrailer) {
  ts.router().setDefault([](const HttpRequest&, HttpResponseWriter& writer) {
    writer.status(http::StatusCodeOK);
    writer.writeBody("chunk1");
    writer.writeBody("chunk2");
    writer.addTrailer("X-Checksum", "abc123");
    writer.end();
  });

  test::ClientConnection sock(port);
  int fd = sock.fd();

  std::string req =
      "GET /stream HTTP/1.1\r\n"
      "Host: example.com\r\n"
      "Connection: close\r\n"
      "\r\n";
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);

  // Check for chunked encoding
  EXPECT_TRUE(resp.contains(MakeHttp1HeaderLine(http::TransferEncoding, "chunked")));

  // Check for chunks
  EXPECT_TRUE(resp.contains("chunk1"));
  EXPECT_TRUE(resp.contains("chunk2"));

  // Check for trailer (appears after the 0-size chunk)
  EXPECT_TRUE(resp.contains("X-Checksum: abc123"));
}

// Test multiple trailers
TEST(HttpResponseWriterTrailers, MultipleTrailers) {
  ts.router().setDefault([](const HttpRequest&, HttpResponseWriter& writer) {
    writer.status(200);
    writer.writeBody("data");
    writer.addTrailer("X-Checksum", "xyz789");
    writer.addTrailer("X-Timestamp", "2025-10-20T12:00:00Z");
    writer.addTrailer("X-Custom", "value");
    writer.end();
  });

  test::ClientConnection sock(port);
  int fd = sock.fd();

  std::string req =
      "GET /multi HTTP/1.1\r\n"
      "Host: example.com\r\n"
      "Connection: close\r\n"
      "\r\n";
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);

  EXPECT_TRUE(resp.contains("X-Checksum: xyz789"));
  EXPECT_TRUE(resp.contains("X-Timestamp: 2025-10-20T12:00:00Z"));
  EXPECT_TRUE(resp.contains("X-Custom: value"));
}

// Test trailer with empty value
TEST(HttpResponseWriterTrailers, EmptyValue) {
  ts.router().setDefault([](const HttpRequest&, HttpResponseWriter& writer) {
    writer.status(http::StatusCodeOK);
    writer.writeBody("test");
    writer.addTrailer("X-Empty", "");
    writer.end();
  });

  test::ClientConnection sock(port);
  int fd = sock.fd();

  std::string req =
      "GET /empty HTTP/1.1\r\n"
      "Host: example.com\r\n"
      "Connection: close\r\n"
      "\r\n";
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);

  // Empty value should still create the header line
  EXPECT_TRUE(resp.contains("X-Empty:"));
}

// Test trailer added after end() is ignored
TEST(HttpResponseWriterTrailers, AfterEndIgnored) {
  ts.router().setDefault([](const HttpRequest&, HttpResponseWriter& writer) {
    writer.status(http::StatusCodeOK);
    writer.writeBody("test");
    writer.end();
    writer.addTrailer("X-Late", "ignored");  // Should be ignored
  });

  test::ClientConnection sock(port);
  int fd = sock.fd();

  std::string req =
      "GET /late HTTP/1.1\r\n"
      "Host: example.com\r\n"
      "Connection: close\r\n"
      "\r\n";
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);

  // Late trailer should NOT appear
  EXPECT_FALSE(resp.contains("X-Late"));
}

// Test trailers ignored for fixed-length responses
TEST(HttpResponseWriterTrailers, IgnoredForFixedLength) {
  ts.router().setDefault([](const HttpRequest&, HttpResponseWriter& writer) {
    writer.status(http::StatusCodeOK);
    writer.contentLength(4);  // Fixed length
    writer.writeBody("test");
    writer.addTrailer("X-Ignored", "value");  // Should be ignored
    writer.end();
  });

  test::ClientConnection sock(port);
  int fd = sock.fd();

  std::string req =
      "GET /fixed HTTP/1.1\r\n"
      "Host: example.com\r\n"
      "Connection: close\r\n"
      "\r\n";
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);

  // Should use Content-Length, not chunked
  EXPECT_TRUE(resp.contains(MakeHttp1HeaderLine(http::ContentLength, "4")));
  EXPECT_FALSE(resp.contains(http::TransferEncoding));

  // Trailer should NOT appear
  EXPECT_FALSE(resp.contains("X-Ignored"));
}

}  // namespace aeronet