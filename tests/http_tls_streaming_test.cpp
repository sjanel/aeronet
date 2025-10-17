#include <gtest/gtest.h>

#include <string>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response-writer.hpp"
#include "aeronet/test_server_tls_fixture.hpp"
#include "aeronet/test_tls_client.hpp"

using namespace std::chrono_literals;

TEST(HttpTlsStreaming, ChunkedSimpleTls) {
  aeronet::test::TlsTestServer ts({"http/1.1"});
  ts.setDefault([]([[maybe_unused]] const aeronet::HttpRequest& req, aeronet::HttpResponseWriter& writer) {
    writer.statusCode(200);
    writer.contentType("text/plain");
    writer.writeBody("hello ");
    writer.writeBody("tls");
    writer.end();
  });
  aeronet::test::TlsClient client(ts.port());
  auto raw = client.get("/stream", {});
  ASSERT_FALSE(raw.empty());
  ASSERT_TRUE(raw.contains("HTTP/1.1 200"));
  ASSERT_TRUE(raw.contains("6\r\nhello "));  // chunk size 6
  ASSERT_TRUE(raw.contains("3\r\ntls"));     // chunk size 3
}
