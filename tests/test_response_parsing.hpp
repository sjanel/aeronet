#pragma once
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "test_http_client.hpp"

namespace testutil {

struct ParsedResponse {
  std::string headersRaw;                      // raw header block including final CRLFCRLF (optional)
  std::map<std::string, std::string> headers;  // parsed key/value pairs
  std::string body;                            // raw body (may be chunked)
  int statusCode{-1};                          // optional (not always populated by all helpers)
};

// Minimal GET request helper used across compression streaming tests. Parses headers into a map and returns body raw.
inline ParsedResponse simpleGet(uint16_t port, std::string_view target,
                                std::vector<std::pair<std::string, std::string>> extraHeaders) {
  test_http_client::RequestOptions opt;
  opt.target = std::string(target);
  opt.headers = std::move(extraHeaders);
  auto rawOpt = test_http_client::request(port, opt);
  if (!rawOpt) {
    throw std::runtime_error("request failed");
  }
  ParsedResponse out;
  const std::string &raw = *rawOpt;
  auto hEnd = raw.find("\r\n\r\n");
  if (hEnd == std::string::npos) {
    throw std::runtime_error("bad response");
  }
  out.headersRaw = raw.substr(0, hEnd + 4);
  auto statusLineEnd = out.headersRaw.find("\r\n");
  if (statusLineEnd != std::string::npos) {
    auto firstSpace = out.headersRaw.find(' ');
    if (firstSpace != std::string::npos) {
      auto secondSpace = out.headersRaw.find(' ', firstSpace + 1);
      std::string codeStr = secondSpace == std::string::npos
                                ? out.headersRaw.substr(firstSpace + 1, statusLineEnd - firstSpace - 1)
                                : out.headersRaw.substr(firstSpace + 1, secondSpace - firstSpace - 1);
      try {
        out.statusCode = std::stoi(codeStr);
      } catch (...) {
        out.statusCode = -1;
      }
    }
  }
  size_t cursor = 0;
  auto nextLine = [&](size_t &pos) {
    auto le = out.headersRaw.find("\r\n", pos);
    if (le == std::string::npos) {
      return std::string_view{};
    }
    std::string_view line(out.headersRaw.data() + pos, le - pos);
    pos = le + 2;
    return line;
  };
  (void)nextLine(cursor);
  while (cursor < out.headersRaw.size()) {
    auto line = nextLine(cursor);
    if (line.empty()) {
      break;
    }
    auto colon = line.find(':');
    if (colon == std::string::npos) {
      continue;
    }
    std::string key(line.substr(0, colon));
    size_t vs = colon + 1;
    while (vs < line.size() && line[vs] == ' ') {
      ++vs;
    }
    std::string val(line.substr(vs));
    out.headers.emplace(std::move(key), std::move(val));
  }
  out.body = raw.substr(hEnd + 4);
  return out;
}

}  // namespace testutil

namespace testutil {
struct ParsedFullResponse {
  int statusCode{};
  std::map<std::string, std::string> headers;
  std::string body;
};

inline ParsedFullResponse doGet(uint16_t port, std::string_view target,
                                std::vector<std::pair<std::string, std::string>> extraHeaders) {
  test_http_client::RequestOptions opt;
  opt.target = std::string(target);
  opt.headers = std::move(extraHeaders);
  auto raw = test_http_client::request(port, opt);
  if (!raw) {
    throw std::runtime_error("request failed");
  }
  ParsedFullResponse out;
  const std::string &rawResp = *raw;
  auto lineEnd = rawResp.find("\r\n");
  if (lineEnd == std::string::npos) {
    throw std::runtime_error("parse failed");
  }
  std::string statusLine = rawResp.substr(0, lineEnd);
  auto firstSpace = statusLine.find(' ');
  if (firstSpace == std::string::npos) {
    throw std::runtime_error("parse failed");
  }
  auto secondSpace = statusLine.find(' ', firstSpace + 1);
  std::string codeStr = secondSpace == std::string::npos
                            ? statusLine.substr(firstSpace + 1)
                            : statusLine.substr(firstSpace + 1, secondSpace - firstSpace - 1);
  out.statusCode = std::atoi(codeStr.c_str());
  auto headersEnd = rawResp.find("\r\n\r\n", lineEnd + 2);
  if (headersEnd == std::string::npos) {
    throw std::runtime_error("parse failed");
  }
  size_t cursor = lineEnd + 2;
  while (cursor < headersEnd) {
    auto le = rawResp.find("\r\n", cursor);
    if (le == std::string::npos || le > headersEnd) {
      break;
    }
    std::string line = rawResp.substr(cursor, le - cursor);
    cursor = le + 2;
    if (line.empty()) {
      break;
    }
    auto colon = line.find(':');
    if (colon == std::string::npos) {
      continue;
    }
    std::string key(line.substr(0, colon));
    size_t vs = colon + 1;
    if (vs < line.size() && line[vs] == ' ') {
      ++vs;
    }
    std::string val(line.substr(vs));
    out.headers[key] = val;
  }
  out.body = rawResp.substr(headersEnd + 4);
  return out;
}
}  // namespace testutil
