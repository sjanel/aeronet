#include <gtest/gtest.h>

#include <string>

#include "aeronet/http-request.hpp"
#include "aeronet/test_server_http2_tls_fixture.hpp"
#include "aeronet/test_tls_http2_client.hpp"

namespace aeronet::test {
namespace {

TEST(TlsHttp2Client, BasicGetRequest) {
  // Create TLS server with HTTP/2 support
  TlsHttp2TestServer ts;
  ts.setDefault([](const HttpRequest& req) {
    return HttpResponse().status(200).body("Hello from HTTP/2 server! Path: " + std::string(req.path()));
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
    return HttpResponse().status(200).body("Request #" + std::to_string(requestCount) + ": " + std::string(req.path()));
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
    return HttpResponse().status(200).body("Received: " + receivedBody);
  });

  TlsHttp2Client client(ts.port());
  ASSERT_TRUE(client.isConnected());

  auto response = client.post("/submit", "Hello, HTTP/2 POST!", "text/plain");
  EXPECT_EQ(response.statusCode, 200);
  EXPECT_EQ(receivedBody, "Hello, HTTP/2 POST!");
  EXPECT_EQ(receivedContentType, "text/plain");
}

TEST(TlsHttp2Client, CustomHeaders) {
  TlsHttp2TestServer ts;
  std::string receivedCustomHeader;
  ts.setDefault([&](const HttpRequest& req) {
    receivedCustomHeader = std::string(req.headerValueOrEmpty("x-custom-header"));
    return HttpResponse().status(200).addHeader("x-response-header", "response-value").body("Headers received");
  });

  TlsHttp2Client client(ts.port());
  ASSERT_TRUE(client.isConnected());

  auto response = client.get("/headers", {{"x-custom-header", "custom-value"}});
  EXPECT_EQ(response.statusCode, 200);
  EXPECT_EQ(receivedCustomHeader, "custom-value");
  EXPECT_EQ(response.header("x-response-header"), "response-value");
}

TEST(TlsHttp2Client, StatusCodes) {
  TlsHttp2TestServer ts;
  ts.setDefault([](const HttpRequest& req) {
    if (req.path() == "/not-found") {
      return HttpResponse().status(404).body("Resource not found");
    }
    if (req.path() == "/error") {
      return HttpResponse().status(500).body("Server error");
    }
    return HttpResponse().status(200).body("Success");
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
    return HttpResponse()
        .status(200)
        .body("Body content")
        .addTrailer("x-checksum", "abc123")
        .addTrailer("x-processing-time-ms", "42");
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
  ts.setDefault([](const HttpRequest& /*req*/) { return HttpResponse().status(200).body("Simple body"); });

  TlsHttp2Client client(ts.port());
  ASSERT_TRUE(client.isConnected());

  auto response = client.get("/simple");
  EXPECT_EQ(response.statusCode, 200);
  EXPECT_EQ(response.body, "Simple body");
}

}  // namespace
}  // namespace aeronet::test
