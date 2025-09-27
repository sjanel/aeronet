#include <gtest/gtest.h>

#include <algorithm>
#include <charconv>
#include <string>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response-writer.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "test_http_client.hpp"
#include "test_server_fixture.hpp"

namespace {
std::string blockingFetch(uint16_t port, const std::string& verb, const std::string& target) {
  test_http_client::RequestOptions opt;
  opt.method = verb;
  opt.target = target;
  opt.connection = "close";
  auto resp = test_http_client::request(port, opt);
  return resp ? *resp : std::string{};
}
}  // namespace

TEST(HttpStreamingAdaptive, CoalescedAndLargePaths) {
  aeronet::HttpServerConfig cfg;
  TestServer ts(cfg);
  auto port = ts.port();
  constexpr std::size_t kLargeSize = 5000;  // > 4096 threshold used in writer
  std::string large(kLargeSize, 'x');
  ts.server.setStreamingHandler([&](const aeronet::HttpRequest&, aeronet::HttpResponseWriter& writer) {
    writer.statusCode(200, "OK");
    writer.write("small");  // coalesced path
    writer.write(large);    // large path (multi enqueue)
    writer.end();
  });
  std::string resp = blockingFetch(port, "GET", "/adaptive");
  auto stats = ts.server.stats();
  ts.stop();
  ASSERT_NE(std::string::npos, resp.find("HTTP/1.1 200"));
  // Validate both chunk headers present: 5 and hex(kLargeSize)
  char hexBuf[32];
  auto res = std::to_chars(hexBuf, hexBuf + sizeof(hexBuf), static_cast<unsigned long long>(kLargeSize), 16);
  ASSERT_TRUE(res.ec == std::errc());
  std::string largeHex(hexBuf, res.ptr);
  ASSERT_NE(std::string::npos, resp.find("5\r\nsmall"));
  ASSERT_NE(std::string::npos, resp.find(largeHex + "\r\n"));
  // Count 'x' occurrences only in the body (after header terminator) to avoid false positives in headers.
  auto hdrEnd = resp.find("\r\n\r\n");
  ASSERT_NE(std::string::npos, hdrEnd);
  std::string_view body(resp.data() + hdrEnd + 4, resp.size() - hdrEnd - 4);
  // Body is chunked: <5 CRLF small CRLF> <hex CRLF largePayload CRLF> 0 CRLF CRLF.
  // We only count 'x' in the large payload; small chunk contains none.
  ASSERT_EQ(kLargeSize, static_cast<size_t>(std::count(body.begin(), body.end(), 'x')));
  // Stats: exactly one coalesced ("small"), one large
  ASSERT_EQ(1u, stats.streamingChunkCoalesced) << "Expected 1 coalesced chunk";
  ASSERT_EQ(1u, stats.streamingChunkLarge) << "Expected 1 large chunk";
}
