#pragma once

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "aeronet/http-constants.hpp"
#include "aeronet/test_util.hpp"
#include "timedef.hpp"

// Lightweight HTTP/1.1 test client helpers with timeouts and safety caps.
// Goals:
//  * Prevent indefinite blocking if the server misbehaves (SO_RCVTIMEO)
//  * Reuse logic across many tests to reduce duplication / maintenance
//  * Provide small convenience routines for simple request/response capture
// Not intended to be a fully compliant client. Only covers scenarios needed by tests.

namespace test_http_client {

struct RequestOptions {
  std::string method{"GET"};
  std::string target{"/"};
  std::string host{"localhost"};
  std::string connection{"close"};
  std::string body;
  std::vector<std::pair<std::string, std::string>> headers;  // additional headers
  int recvTimeoutSeconds{2};                                 // socket receive timeout
  std::size_t maxResponseBytes{1 << 20};                     // 1 MiB safety cap
};

// Minimal parsed HTTP response representation for test assertions.
struct ParsedResponse {
  int statusCode{0};
  std::string reason;
  std::map<std::string, std::string> headers;  // case-sensitive keys (sufficient for tests)
  std::string body;                            // decoded body (if chunked, de-chunked)
  bool chunked{false};
};

inline std::string toLower(std::string input) {
  for (char &ch : input) {
    ch = static_cast<char>(::tolower(static_cast<unsigned char>(ch)));
  }
  return input;
}

// Very small HTTP/1.1 response parser (not resilient to all malformed cases, just for test consumption)
inline std::optional<ParsedResponse> parseResponse(const std::string &raw) {
  ParsedResponse pr;
  std::size_t pos = raw.find(aeronet::http::CRLF);
  if (pos == std::string::npos) {
    return std::nullopt;
  }
  std::string statusLine = raw.substr(0, pos);
  // Expect: HTTP/1.1 <code> <reason>
  auto firstSpace = statusLine.find(' ');
  if (firstSpace == std::string::npos) {
    return std::nullopt;
  }
  auto secondSpace = statusLine.find(' ', firstSpace + 1);
  if (secondSpace == std::string::npos) {
    return std::nullopt;
  }
  pr.statusCode = std::atoi(statusLine.substr(firstSpace + 1, secondSpace - firstSpace - 1).c_str());
  pr.reason = statusLine.substr(secondSpace + 1);
  std::size_t headerEnd = raw.find(aeronet::http::CRLF, pos + aeronet::http::CRLF.size());
  if (headerEnd == std::string::npos) {
    return std::nullopt;
  }
  std::size_t cursor = pos + aeronet::http::CRLF.size();
  while (cursor < headerEnd) {
    std::size_t lineEnd = raw.find(aeronet::http::CRLF, cursor);
    if (lineEnd == std::string::npos || lineEnd > headerEnd) {
      break;
    }
    std::string line = raw.substr(cursor, lineEnd - cursor);
    cursor = lineEnd + aeronet::http::CRLF.size();
    if (line.empty()) {
      break;
    }
    auto colon = line.find(':');
    if (colon == std::string::npos) {
      continue;
    }
    std::string key = line.substr(0, colon);
    // skip space after colon if present
    std::size_t valueStart = colon + 1;
    if (valueStart < line.size() && line[valueStart] == ' ') {
      ++valueStart;
    }
    std::string value = line.substr(valueStart);
    pr.headers[key] = value;
  }
  pr.chunked = false;
  auto teIt = pr.headers.find("Transfer-Encoding");
  if (teIt != pr.headers.end() && toLower(teIt->second).find("chunked") != std::string::npos) {
    pr.chunked = true;
  }
  std::string bodyRaw = raw.substr(headerEnd + 4);
  if (!pr.chunked) {
    pr.body = std::move(bodyRaw);
    return pr;
  }
  // De-chunk (simple algorithm; ignores trailers)
  std::size_t bpos = 0;
  while (bpos < bodyRaw.size()) {
    std::size_t lineEnd = bodyRaw.find(aeronet::http::CRLF, bpos);
    if (lineEnd == std::string::npos) {
      break;
    }
    std::string lenHex = bodyRaw.substr(bpos, lineEnd - bpos);
    bpos = lineEnd + aeronet::http::CRLF.size();
    std::size_t chunkLen = std::strtoul(lenHex.c_str(), nullptr, 16);
    if (chunkLen == 0) {
      break;
    }
    if (bpos + chunkLen > bodyRaw.size()) {
      break;  // malformed / truncated
    }
    pr.body.append(bodyRaw.data() + bpos, chunkLen);
    bpos += chunkLen;
    // expect CRLF
    if (bpos + aeronet::http::CRLF.size() > bodyRaw.size()) {
      break;
    }
    bpos += aeronet::http::CRLF.size();  // skip CRLF
  }
  return pr;
}

// (Removed unused connectLoopback helper; direct socket creation occurs in request())

inline bool setRecvTimeout(int fd, aeronet::Duration timeout) {
  const int timeoutMs = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(timeout).count());
  struct timeval tv{timeoutMs / 1000, static_cast<long>((timeoutMs % 1000) * 1000)};
  return ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0;
}

inline std::string buildRequest(const RequestOptions &opt) {
  std::string req;
  req.reserve(256 + opt.body.size());
  req.append(opt.method).append(" ").append(opt.target).append(" HTTP/1.1\r\n");
  req.append("Host: ").append(opt.host).append(aeronet::http::CRLF);
  req.append("Connection: ").append(opt.connection).append(aeronet::http::CRLF);
  for (auto &header : opt.headers) {
    req.append(header.first).append(aeronet::http::HeaderSep).append(header.second).append(aeronet::http::CRLF);
  }
  if (!opt.body.empty()) {
    req.append("Content-Length: ").append(std::to_string(opt.body.size())).append(aeronet::http::CRLF);
  }
  req.append(aeronet::http::CRLF);
  req.append(opt.body);
  return req;
}

inline std::optional<std::string> request(uint16_t port, const RequestOptions &opt = {}) {
  aeronet::test::ClientConnection cnx(port);
  int fd = cnx.fd();
  setRecvTimeout(fd, std::chrono::seconds(opt.recvTimeoutSeconds));
  auto reqStr = buildRequest(opt);
  ssize_t sent = ::send(fd, reqStr.data(), reqStr.size(), 0);
  if (sent != static_cast<ssize_t>(reqStr.size())) {
    return std::nullopt;
  }
  std::string out;
  static constexpr std::size_t kChunkSize = 4096;

  for (;;) {
    std::size_t oldSize = out.size();

    if (out.capacity() < out.size() + kChunkSize) {
      // ensure exponential growth
      out.reserve(out.capacity() * 2UL);
    }

    out.resize_and_overwrite(out.size() + kChunkSize, [fd, oldSize](char *data, [[maybe_unused]] std::size_t newCap) {
      ssize_t recvBytes = ::recv(fd, data + oldSize, kChunkSize, 0);
      if (recvBytes <= 0) {
        aeronet::log::error("test_http_client::request: recv error or connection closed, errno={}",
                            std::strerror(errno));
        return oldSize;  // timeout or close
      }

      return oldSize + static_cast<std::size_t>(recvBytes);
    });
    if (out.size() == oldSize) {
      break;  // no new data read
    }
  }
  return out;
}

// Convenience wrapper that throws std::runtime_error on failure instead of returning std::nullopt.
// This simplifies test code by eliminating explicit ASSERT checks for has_value(); gtest will treat
// uncaught exceptions as test failures with the diagnostic message.
inline std::string requestOrThrow(uint16_t port, const RequestOptions &opt = {}) {
  auto resp = request(port, opt);
  if (!resp.has_value()) {
    throw std::runtime_error("test_http_client::requestOrThrow: request failed (socket/connect/send/recv) ");
  }
  return std::move(*resp);
}

// Send multiple requests over a single keep-alive connection and return raw responses individually.
// Limitations: assumes server responds fully before next request is parsed (sufficient for simple tests).
inline std::vector<std::string> sequentialRequests(uint16_t port, std::span<const RequestOptions> reqs) {
  std::vector<std::string> results;
  if (reqs.empty()) {
    return results;
  }
  aeronet::test::ClientConnection cnx(port);
  int fd = cnx.fd();
  setRecvTimeout(fd, std::chrono::seconds(reqs.front().recvTimeoutSeconds));

  for (std::size_t i = 0; i < reqs.size(); ++i) {
    RequestOptions ro = reqs[i];
    // For all but last, force keep-alive unless caller explicitly set close.
    if (i + 1 < reqs.size() && ro.connection == "close") {
      ro.connection = "keep-alive";
    }
    std::string rq = buildRequest(ro);
    if (::send(fd, rq.data(), rq.size(), 0) != static_cast<ssize_t>(rq.size())) {
      break;
    }
    // Collect one response (headers + body); naive: read until either connection closes (last) or we have
    // headers+body by heuristic.
    std::string out;
    // We'll read until: if Connection: close seen in headers and header terminator reached & body length satisfied
    // (if Content-Length), or until timeout.
    bool headersDone = false;
    std::size_t contentLen = 0;
    bool haveContentLen = false;
    bool chunked = false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(ro.recvTimeoutSeconds);

    static constexpr std::size_t kChunkSize = 4096;

    while (std::chrono::steady_clock::now() < deadline) {
      std::size_t oldSize = out.size();

      if (out.capacity() < out.size() + kChunkSize) {
        // ensure exponential growth
        out.reserve(out.capacity() * 2UL);
      }

      out.resize_and_overwrite(out.size() + kChunkSize, [fd, oldSize](char *data, [[maybe_unused]] std::size_t newCap) {
        ssize_t recvBytes = ::recv(fd, data + oldSize, kChunkSize, 0);
        if (recvBytes <= 0) {
          aeronet::log::error("test_http_client::request: recv error or connection closed, errno={}",
                              std::strerror(errno));
          return oldSize;  // timeout or close
        }

        return oldSize + static_cast<std::size_t>(recvBytes);
      });

      if (!headersDone) {
        auto hpos = out.find(aeronet::http::DoubleCRLF);
        if (hpos != std::string::npos) {
          headersDone = true;
          // Parse minimal headers for length/chunked
          std::string headerBlock = out.substr(0, hpos + aeronet::http::DoubleCRLF.size());
          std::size_t lineStart = 0;
          while (lineStart < headerBlock.size()) {
            auto lineEnd = headerBlock.find("\r\n", lineStart);
            if (lineEnd == std::string::npos) {
              break;
            }
            if (lineEnd == lineStart) {
              break;  // blank line
            }
            std::string line = headerBlock.substr(lineStart, lineEnd - lineStart);
            lineStart = lineEnd + 2;
            auto colon = line.find(':');
            if (colon == std::string::npos) {
              continue;
            }
            std::string key = line.substr(0, colon);
            std::string val = line.substr(colon + 1);
            if (!val.empty() && val[0] == ' ') {
              val.erase(0, 1);
            }
            std::string keyLower = toLower(key);
            if (keyLower == "content-length") {
              haveContentLen = true;
              contentLen = std::strtoull(val.c_str(), nullptr, 10);
            }
            if (keyLower == "transfer-encoding" && toLower(val).find("chunked") != std::string::npos) {
              chunked = true;
            }
          }
          if (chunked) {
            // For simplicity, read until terminating 0 chunk appears.
            if (out.find("\r\n0\r\n\r\n", hpos + aeronet::http::DoubleCRLF.size()) != std::string::npos) {
              break;
            }
          } else if (haveContentLen) {
            std::size_t bodySoFar = out.size() - (hpos + aeronet::http::DoubleCRLF.size());
            if (bodySoFar >= contentLen) {
              break;
            }
          } else if (ro.connection == "close") {
            // no length framing, rely on connection close -> will continue until server closes or timeout.
          } else {
            // cannot determine length; break to avoid indefinite wait.
            break;
          }
        }
      } else {
        if (chunked) {
          if (out.find("\r\n0\r\n\r\n") != std::string::npos) {
            break;
          }
        } else if (haveContentLen) {
          auto hpos = out.find(aeronet::http::DoubleCRLF);
          if (hpos != std::string::npos) {
            std::size_t bodySoFar = out.size() - (hpos + aeronet::http::DoubleCRLF.size());
            if (bodySoFar >= contentLen) {
              break;
            }
          }
        }
      }
    }
    results.push_back(std::move(out));
    if (ro.connection == "close") {
      break;
    }
  }
  return results;
}

// Incremental streaming helpers: open, send one request, then allow caller to pull available bytes.
struct StreamingHandle {
  aeronet::test::ClientConnection cnx;
};

inline std::optional<StreamingHandle> openStreaming(uint16_t port, const RequestOptions &opt) {
  RequestOptions ro = opt;  // copy
  if (ro.connection == "close") {
    ro.connection = "keep-alive";  // keep open for streaming
  }
  aeronet::test::ClientConnection cnx(port);
  int fd = cnx.fd();
  setRecvTimeout(fd, std::chrono::seconds(ro.recvTimeoutSeconds));
  std::string req = buildRequest(ro);
  if (::send(fd, req.data(), req.size(), 0) != static_cast<ssize_t>(req.size())) {
    return std::nullopt;
  }
  StreamingHandle handle{std::move(cnx)};
  return handle;
}

inline std::string readAvailable(const StreamingHandle &handle) {
  // Use optimized helper: reads immediately available bytes and returns quickly.
  // Small timeout (default 500ms in helper) is truncated early after first EAGAIN.
  return aeronet::test::recvWithTimeout(handle.cnx.fd(), std::chrono::milliseconds(50));
}

}  // namespace test_http_client
