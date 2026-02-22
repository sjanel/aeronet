#include <gtest/gtest.h>

#include <cstddef>
#include <limits>
#include <string>

#include "aeronet/http-constants.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/test_server_http2_tls_fixture.hpp"
#include "aeronet/test_tls_http2_client.hpp"
#include "aeronet/time-constants.hpp"
#include "aeronet/timestring.hpp"

#ifdef AERONET_ENABLE_ZLIB
#include "aeronet/raw-chars.hpp"
#include "aeronet/zlib-decoder.hpp"
#include "aeronet/zlib-encoder.hpp"
#include "aeronet/zlib-stream-raii.hpp"
#endif

namespace aeronet::test {
namespace {

std::string DumpResponseHeaders(const TlsHttp2Client::Response& response) {
  std::string out;
  for (const auto& [name, value] : response.headers) {
    out.append(name);
    out.append(": ");
    out.append(value);
    out.push_back('\n');
  }
  return out;
}

TEST(TlsHttp2Client, BasicGetRequest) {
  // Create TLS server with HTTP/2 support
  TlsHttp2TestServer ts;
  ts.setDefault([](const HttpRequest& req) {
    return HttpResponse("Hello from HTTP/2 server! Path: " + std::string(req.path()));
  });

  // Create HTTP/2 client and verify connection
  TlsHttp2Client client(ts.port());
  ASSERT_TRUE(client.isConnected()) << "Failed to establish HTTP/2 connection";
  EXPECT_EQ(client.negotiatedAlpn(), "h2");

  // Send a GET request
  auto response = client.get("/test-path");
  EXPECT_EQ(response.statusCode, 200);
  EXPECT_TRUE(response.body.contains("Hello from HTTP/2 server!"));
  EXPECT_TRUE(response.body.contains("/test-path"));
}

TEST(TlsHttp2Client, MultipleSequentialRequests) {
  TlsHttp2TestServer ts;
  int requestCount = 0;
  ts.setDefault([&requestCount](const HttpRequest& req) {
    ++requestCount;
    return HttpResponse("Request #" + std::to_string(requestCount) + ": " + std::string(req.path()));
  });

  TlsHttp2Client client(ts.port());
  ASSERT_TRUE(client.isConnected());

  // Send multiple requests on the same connection
  auto resp1 = client.get("/first");
  EXPECT_EQ(resp1.statusCode, 200);
  EXPECT_TRUE(resp1.body.contains("Request #1"));

  auto resp2 = client.get("/second");
  EXPECT_EQ(resp2.statusCode, 200);
  EXPECT_TRUE(resp2.body.contains("Request #2"));

  auto resp3 = client.get("/third");
  EXPECT_EQ(resp3.statusCode, 200);
  EXPECT_TRUE(resp3.body.contains("Request #3"));
}

TEST(TlsHttp2Client, PostRequestWithBody) {
  TlsHttp2TestServer ts;
  std::string receivedBody;
  std::string receivedContentType;
  ts.setDefault([&](const HttpRequest& req) {
    receivedBody = std::string(req.body());
    receivedContentType = std::string(req.headerValueOrEmpty("content-type"));
    return HttpResponse("Received: " + receivedBody);
  });

  TlsHttp2Client client(ts.port());
  ASSERT_TRUE(client.isConnected());

  auto response = client.post("/submit", "Hello, HTTP/2 POST!", "text/plain");
  EXPECT_EQ(response.statusCode, 200);
  EXPECT_EQ(receivedBody, "Hello, HTTP/2 POST!");
  EXPECT_EQ(receivedContentType, "text/plain");
}

#ifdef AERONET_ENABLE_ZLIB
TEST(TlsHttp2Client, AutomaticResponseCompressionRespectsConfig) {
  TlsHttp2TestServer ts([](HttpServerConfig& cfg) {
    cfg.compression.minBytes = 16UL;
    cfg.compression.addVaryAcceptEncodingHeader = true;
  });

  const std::string plainBody(16UL * 1024UL, 'A');
  ts.setDefault([&](const HttpRequest& /*req*/) { return HttpResponse(200, plainBody); });

  TlsHttp2Client client(ts.port());
  ASSERT_TRUE(client.isConnected());

  auto response = client.get("/gzip", {{"accept-encoding", "gzip"}});
  EXPECT_EQ(response.statusCode, 200);
  EXPECT_EQ(response.header("content-encoding"), "gzip");
  EXPECT_EQ(response.header("vary"), http::AcceptEncoding);

  RawChars out;
  ZlibDecoder decoder(ZStreamRAII::Variant::gzip);
  ASSERT_TRUE(decoder.decompressFull(response.body, std::numeric_limits<std::size_t>::max(), 32UL * 1024UL, out));
  EXPECT_EQ(std::string_view(out), plainBody);
}

TEST(TlsHttp2Client, AutomaticRequestDecompressionDeliversCanonicalBody) {
  TlsHttp2TestServer ts;

  std::string receivedBody;
  std::string receivedContentEncoding;
  std::string receivedOriginalEncoding;
  std::string receivedOriginalEncodedLen;
  std::string receivedContentLen;

  ts.setDefault([&](const HttpRequest& req) {
    receivedBody = std::string(req.body());
    receivedContentEncoding = std::string(req.headerValueOrEmpty("content-encoding"));
    receivedOriginalEncoding = std::string(req.headerValueOrEmpty(http::OriginalEncodingHeaderName));
    receivedOriginalEncodedLen = std::string(req.headerValueOrEmpty(http::OriginalEncodedLengthHeaderName));
    receivedContentLen = std::string(req.headerValueOrEmpty("content-length"));
    return HttpResponse("ok");
  });

  const std::string plain = "Hello request decompression over h2";

  CompressionConfig compressionCfg;
  RawChars buf;
  ZlibEncoder encoder(compressionCfg.zlib.level);
  RawChars compressed(64UL + plain.size());
  const auto result = encoder.encodeFull(ZStreamRAII::Variant::gzip, plain, compressed.capacity(), compressed.data());
  ASSERT_FALSE(result.hasError());
  compressed.setSize(result.written());
  const std::string compressedBody(compressed.data(), compressed.size());

  TlsHttp2Client client(ts.port());
  ASSERT_TRUE(client.isConnected());

  auto response =
      client.post("/submit", compressedBody, "application/octet-stream",
                  {{"content-encoding", "gzip"}, {"content-length", std::to_string(compressedBody.size())}});
  EXPECT_EQ(response.statusCode, 200);

  EXPECT_EQ(receivedBody, plain);
  EXPECT_TRUE(receivedContentEncoding.empty());
  EXPECT_EQ(receivedOriginalEncoding, "gzip");
  EXPECT_EQ(receivedOriginalEncodedLen, std::to_string(compressedBody.size()));
  EXPECT_EQ(receivedContentLen, std::to_string(plain.size()));
}
#endif

TEST(TlsHttp2Client, CustomHeaders) {
  TlsHttp2TestServer ts;
  std::string receivedCustomHeader;
  ts.setDefault([&](const HttpRequest& req) {
    receivedCustomHeader = std::string(req.headerValueOrEmpty("x-custom-header"));
    return HttpResponse().headerAddLine("x-response-header", "response-value").body("Headers received");
  });

  TlsHttp2Client client(ts.port());
  ASSERT_TRUE(client.isConnected());

  auto response = client.get("/headers", {{"x-custom-header", "custom-value"}});
  EXPECT_EQ(response.statusCode, 200);
  EXPECT_EQ(receivedCustomHeader, "custom-value");
  EXPECT_EQ(response.header("x-response-header"), "response-value");
}

TEST(TlsHttp2Client, GlobalHeadersAndDateAreInjected) {
  TlsHttp2TestServer ts([](HttpServerConfig& cfg) {
    cfg.addGlobalHeader(http::Header{"X-Global", "gvalue"});
    cfg.addGlobalHeader(http::Header{"X-Another", "anothervalue"});
    cfg.addGlobalHeader(http::Header{"X-Custom", "global"});
  });

  ts.setDefault([](const HttpRequest& /*req*/) {
    HttpResponse resp;
    resp.headerAddLine("x-custom", "original");
    resp.body("R");
    return resp;
  });

  TlsHttp2Client client(ts.port());
  ASSERT_TRUE(client.isConnected());

  auto response = client.get("/global-headers");
  EXPECT_EQ(response.statusCode, 200);

  EXPECT_EQ(response.header("x-global"), "gvalue");
  EXPECT_EQ(response.header("x-another"), "anothervalue");
  EXPECT_EQ(response.header("x-custom"), "original");

  const auto date = response.header("date");
  ASSERT_FALSE(date.empty()) << "Received headers:\n" << DumpResponseHeaders(response);
  EXPECT_EQ(date.size(), RFC7231DateStrLen);
  EXPECT_TRUE(date.ends_with("GMT"));
  EXPECT_NE(TryParseTimeRFC7231(date), kInvalidTimePoint);
}

TEST(TlsHttp2Client, MakeResponsePrefillsGlobalHeaders) {
  TlsHttp2TestServer ts([](HttpServerConfig& cfg) {
    cfg.addGlobalHeader(http::Header{"X-Global", "gvalue"});
    cfg.addGlobalHeader(http::Header{"X-Another", "anothervalue"});
  });

  ts.setDefault([](const HttpRequest& req) {
    auto resp = req.makeResponse(http::StatusCodeAccepted, "h2-body", "text/custom");
    resp.header("X-Local", "local-value");
    return resp;
  });

  TlsHttp2Client client(ts.port());
  ASSERT_TRUE(client.isConnected());

  auto response = client.get("/make-response");
  EXPECT_EQ(response.statusCode, 202);
  EXPECT_EQ(response.body, "h2-body");
  EXPECT_EQ(response.header("x-global"), "gvalue");
  EXPECT_EQ(response.header("x-another"), "anothervalue");
  EXPECT_EQ(response.header("x-local"), "local-value");
}

TEST(TlsHttp2Client, HeadOmitsBodyButSetsContentLengthAndDate) {
  TlsHttp2TestServer ts;
  ts.setDefault([](const HttpRequest& /*req*/) { return HttpResponse("abc"); });

  TlsHttp2Client client(ts.port());
  ASSERT_TRUE(client.isConnected());

  auto response = client.request("HEAD", "/head-test");
  EXPECT_EQ(response.statusCode, 200);
  EXPECT_TRUE(response.body.empty());
  EXPECT_EQ(response.header("content-length"), "3");

  const auto date = response.header("date");
  EXPECT_EQ(date.size(), RFC7231DateStrLen);
  EXPECT_TRUE(date.ends_with("GMT"));
}

TEST(TlsHttp2Client, StatusCodes) {
  TlsHttp2TestServer ts;
  ts.setDefault([](const HttpRequest& req) {
    if (req.path() == "/not-found") {
      return HttpResponse(404, "Resource not found");
    }
    if (req.path() == "/error") {
      return HttpResponse(500, "Server error");
    }
    return HttpResponse("Success");
  });

  TlsHttp2Client client(ts.port());
  ASSERT_TRUE(client.isConnected());

  auto ok = client.get("/");
  EXPECT_EQ(ok.statusCode, 200);

  auto notFound = client.get("/not-found");
  EXPECT_EQ(notFound.statusCode, 404);

  auto error = client.get("/error");
  EXPECT_EQ(error.statusCode, 500);
}

TEST(TlsHttp2Client, TrailersAreSentAfterBody) {
  // Test that HTTP/2 trailers are correctly sent as a HEADERS frame with END_STREAM after DATA frames
  TlsHttp2TestServer ts;
  ts.setDefault([](const HttpRequest& /*req*/) {
    return HttpResponse("Body content")
        .trailerAddLine("x-checksum", "abc123")
        .trailerAddLine("x-processing-time-ms", "42");
  });

  TlsHttp2Client client(ts.port());
  ASSERT_TRUE(client.isConnected());

  auto response = client.get("/with-trailers");
  EXPECT_EQ(response.statusCode, 200);
  EXPECT_EQ(response.body, "Body content");

  // Trailers should appear in the headers list (HTTP/2 trailers are sent as a final HEADERS frame)
  EXPECT_EQ(response.header("x-checksum"), "abc123");
  EXPECT_EQ(response.header("x-processing-time-ms"), "42");
}

TEST(TlsHttp2Client, ResponseWithoutBodyNoTrailers) {
  TlsHttp2TestServer ts;
  ts.setDefault([](const HttpRequest& /*req*/) { return HttpResponse().status(204); });

  TlsHttp2Client client(ts.port());
  ASSERT_TRUE(client.isConnected());

  auto response = client.get("/no-content");
  EXPECT_EQ(response.statusCode, 204);
  EXPECT_TRUE(response.body.empty());
}

TEST(TlsHttp2Client, ResponseWithBodyNoTrailers) {
  TlsHttp2TestServer ts;
  ts.setDefault([](const HttpRequest& /*req*/) { return HttpResponse("Simple body"); });

  TlsHttp2Client client(ts.port());
  ASSERT_TRUE(client.isConnected());

  auto response = client.get("/simple");
  EXPECT_EQ(response.statusCode, 200);
  EXPECT_EQ(response.body, "Simple body");
}

}  // namespace
}  // namespace aeronet::test
