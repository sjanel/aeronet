#include <gtest/gtest.h>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string>
#include <system_error>

#include "aeronet/http-constants.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response-writer.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_util.hpp"

namespace {
std::string blockingFetch(uint16_t port, const std::string& verb, const std::string& target) {
  aeronet::test::RequestOptions opt;
  opt.method = verb;
  opt.target = target;
  opt.connection = "close";
  auto resp = aeronet::test::request(port, opt);
  return resp ? *resp : std::string{};
}
}  // namespace

TEST(HttpStreamingAdaptive, CoalescedAndLargePaths) {
  constexpr std::size_t kLargeSize = 5000;

  aeronet::HttpServerConfig cfg;
  cfg.minCapturedBodySize = kLargeSize - 1U;
  aeronet::test::TestServer ts(cfg);
  auto port = ts.port();
  std::string large(kLargeSize, 'x');
  ts.server.router().setDefault([&](const aeronet::HttpRequest&, aeronet::HttpResponseWriter& writer) {
    writer.statusCode(200);
    writer.writeBody("small");  // coalesced path
    writer.writeBody(large);    // large path (multi enqueue)
    writer.end();
  });
  std::string resp = blockingFetch(port, "GET", "/adaptive");
  auto stats = ts.server.stats();
  ts.stop();
  ASSERT_TRUE(resp.contains("HTTP/1.1 200"));
  // Validate both chunk headers present: 5 and hex(kLargeSize)
  char hexBuf[32];
  auto res = std::to_chars(hexBuf, hexBuf + sizeof(hexBuf), static_cast<unsigned long long>(kLargeSize), 16);
  ASSERT_TRUE(res.ec == std::errc());
  std::string largeHex(hexBuf, res.ptr);
  ASSERT_TRUE(resp.contains("5\r\nsmall"));
  ASSERT_TRUE(resp.contains(largeHex + "\r\n"));
  // Count 'x' occurrences only in the body (after header terminator) to avoid false positives in headers.
  auto hdrEnd = resp.find(aeronet::http::DoubleCRLF);
  ASSERT_NE(std::string::npos, hdrEnd);
  std::string_view body(resp.data() + hdrEnd + aeronet::http::DoubleCRLF.size(),
                        resp.size() - hdrEnd - aeronet::http::DoubleCRLF.size());
  // Body is chunked: <5 CRLF small CRLF> <hex CRLF largePayload CRLF> 0 CRLF CRLF.
  // We only count 'x' in the large payload; small chunk contains none.
  ASSERT_EQ(kLargeSize, static_cast<size_t>(std::count(body.begin(), body.end(), 'x')));
}
