#include <gtest/gtest.h>
#include <zstd.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aeronet/compression-config.hpp"
#include "aeronet/encoding.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "simple-charconv.hpp"
#include "test_http_client.hpp"
#include "test_server_fixture.hpp"
#include "zstd_test_helpers.hpp"

using namespace aeronet;

namespace {
struct ParsedFullResponse {
  int statusCode{};
  std::map<std::string, std::string> headers;
  std::string body;
};
ParsedFullResponse doGet(uint16_t port, std::string_view target,
                         std::vector<std::pair<std::string, std::string>> extra) {
  test_http_client::RequestOptions opt;
  opt.target = std::string(target);
  opt.headers = std::move(extra);
  auto raw = test_http_client::request(port, opt);
  if (!raw) {
    throw std::runtime_error("request failed");
  }
  ParsedFullResponse out;
  const std::string& rawResp = *raw;
  auto lineEnd = rawResp.find("\r\n");
  if (lineEnd == std::string::npos) {
    throw std::runtime_error("status parse");
  }
  auto firstSpace = rawResp.find(' ');
  auto secondSpace = rawResp.find(' ', firstSpace + 1);
  auto codeStr = secondSpace == std::string::npos ? rawResp.substr(firstSpace + 1)
                                                  : rawResp.substr(firstSpace + 1, secondSpace - firstSpace - 1);
  out.statusCode = read3(codeStr.data());
  auto headersEnd = rawResp.find("\r\n\r\n", lineEnd + 2);
  if (headersEnd == std::string::npos) {
    throw std::runtime_error("hdr parse");
  }
  size_t cur = lineEnd + 2;
  while (cur < headersEnd) {
    auto le = rawResp.find("\r\n", cur);
    if (le == std::string::npos || le > headersEnd) {
      break;
    }
    std::string line = rawResp.substr(cur, le - cur);
    cur = le + 2;
    if (line.empty()) {
      break;
    }
    auto colon = line.find(':');
    if (colon == std::string::npos) {
      continue;
    }
    std::string key = line.substr(0, colon);
    size_t vs = colon + 1;
    if (vs < line.size() && line[vs] == ' ') {
      ++vs;
    }
    std::string val = line.substr(vs);
    out.headers[key] = val;
  }
  out.body = rawResp.substr(headersEnd + 4);
  return out;
}

bool HasZstdMagic(std::string_view body) {
  // zstd frame magic little endian 0x28 B5 2F FD
  return body.size() >= 4 && static_cast<unsigned char>(body[0]) == 0x28 &&
         static_cast<unsigned char>(body[1]) == 0xB5 && static_cast<unsigned char>(body[2]) == 0x2F &&
         static_cast<unsigned char>(body[3]) == 0xFD;
}
}  // namespace

TEST(HttpCompressionZstdBuffered, ZstdAppliedWhenEligible) {
  CompressionConfig cfg;
  cfg.minBytes = 32;
  cfg.preferredFormats.push_back(Encoding::zstd);
  HttpServerConfig scfg{};
  scfg.withCompression(cfg);
  TestServer ts(std::move(scfg));
  std::string payload(400, 'A');
  ts.server.setHandler([payload](const HttpRequest&) {
    HttpResponse resp;
    resp.customHeader("Content-Type", "text/plain");
    resp.body(payload);
    return resp;
  });
  auto resp = doGet(ts.port(), "/z", {{"Accept-Encoding", "zstd"}});
  ASSERT_EQ(resp.statusCode, 200);
  auto it = resp.headers.find("Content-Encoding");
  ASSERT_NE(it, resp.headers.end());
  EXPECT_EQ(it->second, "zstd");
  EXPECT_TRUE(HasZstdMagic(resp.body));
  EXPECT_LT(resp.body.size(), payload.size());
  // Round-trip verify by decompressing (simple one-shot) to ensure integrity
  std::string decompressed = aeronet::test::zstdRoundTripDecompress(resp.body, payload.size());
  EXPECT_EQ(decompressed, payload);
}

TEST(HttpCompressionZstdBuffered, WildcardSelectsZstdIfPreferred) {
  CompressionConfig cfg;
  cfg.minBytes = 16;
  cfg.preferredFormats.push_back(Encoding::zstd);
  cfg.preferredFormats.push_back(Encoding::gzip);
  HttpServerConfig scfg{};
  scfg.withCompression(cfg);
  TestServer ts(std::move(scfg));
  std::string payload(256, 'B');
  ts.server.setHandler([payload](const HttpRequest&) {
    HttpResponse resp;
    resp.body(payload);
    resp.customHeader("Content-Type", "text/plain");
    return resp;
  });
  auto resp = doGet(ts.port(), "/w", {{"Accept-Encoding", "*;q=0.9"}});
  auto it = resp.headers.find("Content-Encoding");
  ASSERT_NE(it, resp.headers.end());
  EXPECT_EQ(it->second, "zstd");
  EXPECT_TRUE(HasZstdMagic(resp.body));
}

TEST(HttpCompressionZstdBuffered, TieBreakAgainstGzipHigherQ) {
  CompressionConfig cfg;
  cfg.minBytes = 16;
  cfg.preferredFormats.push_back(Encoding::zstd);
  cfg.preferredFormats.push_back(Encoding::gzip);
  HttpServerConfig scfg{};
  scfg.withCompression(cfg);
  TestServer ts(std::move(scfg));
  std::string payload(512, 'C');
  ts.server.setHandler([payload](const HttpRequest&) {
    HttpResponse resp;
    resp.body(payload);
    resp.customHeader("Content-Type", "text/plain");
    return resp;
  });
  auto resp = doGet(ts.port(), "/t", {{"Accept-Encoding", "gzip;q=0.9, zstd;q=0.9"}});
  auto it = resp.headers.find("Content-Encoding");
  ASSERT_NE(it, resp.headers.end());
  EXPECT_EQ(it->second, "zstd");
}
