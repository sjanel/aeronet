#include <gtest/gtest.h>
#include <zstd.h>

#include <charconv>
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
#include "aeronet/http-response-writer.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "aeronet/http-status-code.hpp"
#include "stringconv.hpp"
#include "test_http_client.hpp"
#include "test_server_fixture.hpp"
#include "zstd_test_helpers.hpp"

using namespace aeronet;

namespace {
struct ParsedResponse {
  std::map<std::string, std::string> headers;
  std::string body;       // raw (possibly chunked) body
  std::string plainBody;  // de-chunked payload (available if Transfer-Encoding: chunked)
  int status{};
};

std::string dechunk(std::string_view raw) {
  std::string out;
  size_t cursor = 0;
  while (cursor < raw.size()) {
    auto lineEnd = raw.find("\r\n", cursor);
    if (lineEnd == std::string::npos) {
      break;  // malformed
    }
    std::string_view sizeLine = raw.substr(cursor, lineEnd - cursor);
    cursor = lineEnd + 2;
    // size may include optional chunk extensions after ';'
    auto sc = sizeLine.find(';');
    if (sc != std::string::npos) {
      sizeLine = sizeLine.substr(0, sc);
    }
    size_t sz = 0;
    if (sizeLine.empty()) {
      return {};  // malformed
    }
    // Use std::from_chars as a compact char->hex conversion (base 16) instead of a manual loop.
    const char *first = sizeLine.data();
    const char *last = first + sizeLine.size();
    auto conv = std::from_chars(first, last, sz, 16);
    if (conv.ec != std::errc() || conv.ptr != last) {
      return {};  // malformed / invalid hex sequence
    }
    if (sz == 0) {
      // Consume trailing CRLF after last chunk (and optional trailer headers which we ignore)
      return out;
    }
    if (cursor + sz + 2 > raw.size()) {
      return {};  // malformed
    }
    out.append(raw.substr(cursor, sz));
    cursor += sz;
    if (raw[cursor] != '\r' || raw[cursor + 1] != '\n') {
      return {};  // malformed
    }
    cursor += 2;
  }
  return out;  // best effort
}
ParsedResponse simpleGet(uint16_t port, std::string_view target,
                         std::vector<std::pair<std::string, std::string>> extra) {
  test_http_client::RequestOptions opt;
  opt.target = std::string(target);
  opt.headers = std::move(extra);
  auto raw = test_http_client::request(port, opt);
  if (!raw) {
    throw std::runtime_error("request failed");
  }
  ParsedResponse out;
  const std::string &respStr = *raw;
  auto headersEnd = respStr.find("\r\n\r\n");
  if (headersEnd == std::string::npos) {
    throw std::runtime_error("bad resp");
  }
  auto statusLineEnd = respStr.find("\r\n");
  auto codeStart = respStr.find(' ');
  auto codeEnd = respStr.find(' ', codeStart + 1);
  out.status = StringToIntegral<http::StatusCode>(respStr.substr(codeStart + 1, codeEnd - codeStart - 1));
  size_t cursor = statusLineEnd + 2;
  while (cursor < headersEnd) {
    auto lineEnd = respStr.find("\r\n", cursor);
    if (lineEnd == std::string::npos || lineEnd > headersEnd) {
      break;
    }
    std::string line = respStr.substr(cursor, lineEnd - cursor);
    cursor = lineEnd + 2;
    if (line.empty()) {
      break;
    }
    auto colon = line.find(':');
    if (colon == std::string::npos) {
      continue;
    }
    std::string key = line.substr(0, colon);
    size_t valueStart = colon + 1;
    if (valueStart < line.size() && line[valueStart] == ' ') {
      ++valueStart;
    }
    std::string val = line.substr(valueStart);
    out.headers[key] = val;
  }
  out.body = respStr.substr(headersEnd + 4);
  auto te = out.headers.find("Transfer-Encoding");
  if (te != out.headers.end() && te->second == "chunked") {
    out.plainBody = dechunk(out.body);
  } else {
    out.plainBody = out.body;
  }
  return out;
}
bool HasZstdMagic(std::string_view body) {
  return body.size() >= 4 && static_cast<unsigned char>(body[0]) == 0x28 &&
         static_cast<unsigned char>(body[1]) == 0xB5 && static_cast<unsigned char>(body[2]) == 0x2F &&
         static_cast<unsigned char>(body[3]) == 0xFD;
}
}  // namespace

TEST(HttpCompressionZstdStreaming, ZstdActivatesAfterThreshold) {
  CompressionConfig cfg;
  cfg.minBytes = 128;
  cfg.preferredFormats.push_back(Encoding::zstd);
  HttpServerConfig scfg{};
  scfg.withCompression(cfg);
  TestServer ts(std::move(scfg));
  std::string chunk1(64, 'x');
  std::string chunk2(128, 'y');
  ts.server.setStreamingHandler([&](const HttpRequest &, HttpResponseWriter &writer) {
    writer.statusCode(200);
    writer.contentType("text/plain");
    writer.write(chunk1);
    writer.write(chunk2);
    writer.end();
  });
  auto resp = simpleGet(ts.port(), "/zs", {{"Accept-Encoding", "zstd"}});
  auto it = resp.headers.find("Content-Encoding");
  ASSERT_NE(it, resp.headers.end());
  EXPECT_EQ(it->second, "zstd");
  EXPECT_TRUE(HasZstdMagic(resp.plainBody));
  // Round-trip decompression via helper
  std::string original = chunk1 + chunk2;
  auto decompressed = aeronet::test::zstdRoundTripDecompress(resp.plainBody, original.size());
  EXPECT_EQ(decompressed, original);
}

TEST(HttpCompressionZstdStreaming, BelowThresholdIdentity) {
  CompressionConfig cfg;
  cfg.minBytes = 1024;
  cfg.preferredFormats.push_back(Encoding::zstd);
  HttpServerConfig scfg{};
  scfg.withCompression(cfg);
  TestServer ts(std::move(scfg));
  std::string data(200, 'a');
  ts.server.setStreamingHandler([&](const HttpRequest &, HttpResponseWriter &writer) {
    writer.statusCode(200);
    writer.contentType("text/plain");
    writer.write(data);
    writer.end();
  });
  auto resp = simpleGet(ts.port(), "/zi", {{"Accept-Encoding", "zstd"}});
  auto it = resp.headers.find("Content-Encoding");
  EXPECT_TRUE(it == resp.headers.end());  // identity
  EXPECT_TRUE(resp.plainBody == data) << "identity path should match input exactly";
}
