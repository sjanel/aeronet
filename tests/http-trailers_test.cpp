#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response-writer.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_util.hpp"

using namespace std::chrono_literals;
using namespace aeronet;

// Basic trailer parsing test
TEST(HttpTrailers, BasicTrailer) {
  test::TestServer ts(HttpServerConfig{});
  auto port = ts.port();
  ts.server.router().setDefault([](const HttpRequest& req) {
    EXPECT_EQ(req.body(), "Wikipedia");
    // Check trailer headers
    EXPECT_EQ(req.trailers().size(), 1U);
    auto it = req.trailers().find("X-Checksum");
    EXPECT_NE(it, req.trailers().end());
    if (it != req.trailers().end()) {
      EXPECT_EQ(it->second, "abc123");
    }
    return HttpResponse(http::StatusCodeOK).body("OK");
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
  ASSERT_TRUE(resp.contains("200"));
}

// Multiple trailer headers
TEST(HttpTrailers, MultipleTrailers) {
  test::TestServer ts(HttpServerConfig{});
  auto port = ts.port();
  ts.server.router().setDefault([](const HttpRequest& req) {
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

    return HttpResponse(http::StatusCodeOK).body("OK");
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
  test::TestServer ts(HttpServerConfig{});
  auto port = ts.port();
  ts.server.router().setDefault([](const HttpRequest& req) {
    EXPECT_EQ(req.body(), "data");
    EXPECT_TRUE(req.trailers().empty());
    return HttpResponse(http::StatusCodeOK).body("OK");
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
  test::TestServer ts(HttpServerConfig{});
  auto port = ts.port();
  ts.server.router().setDefault([](const HttpRequest& req) {
    auto trailer = req.trailers().find("X-Data");
    EXPECT_NE(trailer, req.trailers().end());
    if (trailer != req.trailers().end()) {
      EXPECT_EQ(trailer->second, "trimmed");  // should be trimmed
    }
    return HttpResponse(http::StatusCodeOK).body("OK");
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
  test::TestServer ts(HttpServerConfig{});
  auto port = ts.port();
  ts.server.router().setDefault([](const HttpRequest&) {
    ADD_FAILURE() << "Handler should not be called for forbidden trailer";
    return HttpResponse(http::StatusCodeOK).body("FAIL");
  });

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
  ASSERT_TRUE(resp.contains("400"));
}

// Forbidden trailer: Content-Length
TEST(HttpTrailers, ForbiddenTrailerContentLength) {
  test::TestServer ts(HttpServerConfig{});
  auto port = ts.port();
  ts.server.router().setDefault([](const HttpRequest&) {
    ADD_FAILURE() << "Handler should not be called for forbidden trailer";
    return HttpResponse(http::StatusCodeOK).body("FAIL");
  });

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
  ASSERT_TRUE(resp.contains("400"));
}

// Forbidden trailer: Host
TEST(HttpTrailers, ForbiddenTrailerHost) {
  test::TestServer ts(HttpServerConfig{});
  auto port = ts.port();
  ts.server.router().setDefault([](const HttpRequest&) {
    ADD_FAILURE() << "Handler should not be called for forbidden trailer";
    return HttpResponse(http::StatusCodeOK).body("FAIL");
  });

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
  ASSERT_TRUE(resp.contains("400"));
}

// Forbidden trailer: Authorization
TEST(HttpTrailers, ForbiddenTrailerAuthorization) {
  test::TestServer ts(HttpServerConfig{});
  auto port = ts.port();
  ts.server.router().setDefault([](const HttpRequest&) {
    ADD_FAILURE() << "Handler should not be called for forbidden trailer";
    return HttpResponse(http::StatusCodeOK).body("FAIL");
  });

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
  ASSERT_TRUE(resp.contains("400"));
}

// Trailer size exceeds limit
TEST(HttpTrailers, TrailerSizeLimit) {
  HttpServerConfig cfg;
  cfg.withMaxHeaderBytes(200);  // reasonable small limit
  test::TestServer ts(cfg);
  auto port = ts.port();
  ts.server.router().setDefault([](const HttpRequest&) {
    ADD_FAILURE() << "Handler should not be called when trailer size exceeded";
    return HttpResponse(http::StatusCodeOK).body("FAIL");
  });

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
  ASSERT_TRUE(resp.contains("431"));
}

// Trailer with empty value
TEST(HttpTrailers, TrailerEmptyValue) {
  test::TestServer ts(HttpServerConfig{});
  auto port = ts.port();
  ts.server.router().setDefault([](const HttpRequest& req) {
    auto trailer = req.trailers().find("X-Empty");
    EXPECT_NE(trailer, req.trailers().end());
    if (trailer != req.trailers().end()) {
      EXPECT_TRUE(trailer->second.empty());
    }
    return HttpResponse(http::StatusCodeOK).body("OK");
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
  test::TestServer ts(HttpServerConfig{});
  auto port = ts.port();
  ts.server.router().setDefault([](const HttpRequest& req) {
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
    return HttpResponse(http::StatusCodeOK).body("OK");
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
  test::TestServer ts(HttpServerConfig{});
  auto port = ts.port();
  ts.server.router().setDefault([](const HttpRequest& req) {
    // Accept header should be merged with a comma separator
    auto it = req.trailers().find("Accept");
    EXPECT_NE(it, req.trailers().end());
    if (it != req.trailers().end()) {
      EXPECT_EQ(it->second, "text/html,application/json");
    }
    return HttpResponse(http::StatusCodeOK).body("OK");
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
  HttpServerConfig cfg;
  test::TestServer ts(cfg);
  auto port = ts.port();
  ts.server.router().setDefault([](const HttpRequest& req) {
    auto it = req.trailers().find("From");
    EXPECT_NE(it, req.trailers().end());
    if (it != req.trailers().end()) {
      // 'From' has override semantics in ReqHeaderValueSeparator, keep the last occurrence
      EXPECT_EQ(it->second, "b@example.com");
    }
    return HttpResponse(http::StatusCodeOK).body("OK");
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
  HttpServerConfig cfg;
  cfg.withMergeUnknownRequestHeaders(false);
  test::TestServer ts(cfg);
  auto port = ts.port();
  ts.server.router().setDefault([](const HttpRequest&) {
    ADD_FAILURE() << "Handler should not be called when unknown-header duplicates are forbidden";
    return HttpResponse(http::StatusCodeOK).body("FAIL");
  });

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
  ASSERT_TRUE(resp.contains("400"));
}

// Malformed trailer (no colon)
TEST(HttpTrailers, MalformedTrailerNoColon) {
  test::TestServer ts(HttpServerConfig{});
  auto port = ts.port();
  ts.server.router().setDefault([](const HttpRequest&) {
    ADD_FAILURE() << "Handler should not be called for malformed trailer";
    return HttpResponse(http::StatusCodeOK).body("FAIL");
  });

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
  ASSERT_TRUE(resp.contains("400"));
}

// Non-chunked request should have empty trailers
TEST(HttpTrailers, NonChunkedNoTrailers) {
  test::TestServer ts(HttpServerConfig{});
  auto port = ts.port();
  ts.server.router().setDefault([](const HttpRequest& req) {
    EXPECT_EQ(req.body(), "test");
    EXPECT_TRUE(req.trailers().empty());
    return HttpResponse(http::StatusCodeOK).body("OK");
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

// Basic trailer test - verify trailers are appended after body
TEST(HttpResponseTrailers, BasicTrailer) {
  HttpResponse resp(http::StatusCodeOK);
  resp.body("test body");
  resp.addTrailer("X-Checksum", "abc123");

  // We can't easily test the serialized output without finalizing,
  // but we can verify no exception is thrown
  EXPECT_NO_THROW(resp.addTrailer("X-Signature", "sha256:..."));
}

// Test error when adding trailer before body
TEST(HttpResponseTrailers, ErrorBeforeBody) {
  HttpResponse resp(http::StatusCodeOK);
  EXPECT_THROW(resp.addTrailer("X-Checksum", "abc123"), std::logic_error);
}

// Test error when adding trailer after an explicitly empty body
TEST(HttpResponseTrailers, EmptyBodyThrows) {
  HttpResponse resp(http::StatusCodeOK);
  resp.body("");  // empty body set explicitly
  EXPECT_THROW(resp.addTrailer("X-Checksum", "abc123"), std::logic_error);
}

// Test trailer with captured body (std::string)
TEST(HttpResponseTrailers, CapturedBodyString) {
  HttpResponse resp(http::StatusCodeOK);
  resp.body(std::string("captured body content"));
  EXPECT_NO_THROW(resp.addTrailer("X-Custom", "value"));
}

// Test trailer with captured body (std::vector<char>)
TEST(HttpResponseTrailers, CapturedBodyVector) {
  HttpResponse resp(http::StatusCodeOK);
  std::vector<char> vec = {'h', 'e', 'l', 'l', 'o'};
  resp.body(std::move(vec));
  EXPECT_NO_THROW(resp.addTrailer("X-Data", "123"));
}

// Test multiple trailers
TEST(HttpResponseTrailers, MultipleTrailers) {
  HttpResponse resp(http::StatusCodeOK);
  resp.body("body");
  resp.addTrailer("X-Checksum", "abc");
  resp.addTrailer("X-Timestamp", "2025-10-20T12:00:00Z");
  resp.addTrailer("X-Custom", "val");
  // No assertion - just verify no crashes
}

// Test empty trailer value
TEST(HttpResponseTrailers, EmptyValue) {
  HttpResponse resp(http::StatusCodeOK);
  resp.body("test");
  EXPECT_NO_THROW(resp.addTrailer("X-Empty", ""));
}

// Test rvalue ref version
TEST(HttpResponseTrailers, RvalueRef) {
  EXPECT_NO_THROW(HttpResponse(http::StatusCodeOK).body("test").addTrailer("X-Check", "val"));
}

// Test that setting the body after inserting a trailer throws
TEST(HttpResponseTrailers, BodyAfterTrailerThrows) {
  HttpResponse resp(http::StatusCodeOK);
  resp.body("initial");
  resp.addTrailer("X-After", "v");
  // setting inline body after trailer insertion should throw
  EXPECT_THROW(resp.body("later"), std::logic_error);
  // setting captured string body after trailer insertion should also throw
  EXPECT_THROW(resp.body(std::string_view("later2")), std::logic_error);
}

// Test streaming response with trailers
TEST(HttpResponseWriterTrailers, BasicStreamingTrailer) {
  test::TestServer ts(HttpServerConfig{});
  auto port = ts.port();

  ts.server.router().setDefault([](const HttpRequest&, HttpResponseWriter& writer) {
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
  EXPECT_TRUE(resp.contains("Transfer-Encoding: chunked"));

  // Check for chunks
  EXPECT_TRUE(resp.contains("chunk1"));
  EXPECT_TRUE(resp.contains("chunk2"));

  // Check for trailer (appears after the 0-size chunk)
  EXPECT_TRUE(resp.contains("X-Checksum: abc123"));
}

// Test multiple trailers
TEST(HttpResponseWriterTrailers, MultipleTrailers) {
  test::TestServer ts(HttpServerConfig{});
  auto port = ts.port();

  ts.server.router().setDefault([](const HttpRequest&, HttpResponseWriter& writer) {
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
  test::TestServer ts(HttpServerConfig{});
  auto port = ts.port();

  ts.server.router().setDefault([](const HttpRequest&, HttpResponseWriter& writer) {
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
  test::TestServer ts(HttpServerConfig{});
  auto port = ts.port();

  ts.server.router().setDefault([](const HttpRequest&, HttpResponseWriter& writer) {
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
  test::TestServer ts(HttpServerConfig{});
  auto port = ts.port();

  ts.server.router().setDefault([](const HttpRequest&, HttpResponseWriter& writer) {
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
  EXPECT_TRUE(resp.contains("Content-Length: 4"));
  EXPECT_FALSE(resp.contains("Transfer-Encoding: chunked"));

  // Trailer should NOT appear
  EXPECT_FALSE(resp.contains("X-Ignored"));
}