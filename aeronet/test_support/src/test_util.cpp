#include "aeronet/test_util.hpp"

#include <asm-generic/socket.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>

#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "aeronet/http-constants.hpp"
#include "aeronet/http-status-code.hpp"
#include "log.hpp"
#include "simple-charconv.hpp"
#include "socket.hpp"
#include "stringconv.hpp"
#include "timedef.hpp"
#include "toupperlower.hpp"

namespace aeronet::test {
using namespace std::chrono_literals;

bool sendAll(int fd, std::string_view data, std::chrono::milliseconds totalTimeout) {
  const char *cursor = data.data();
  std::size_t remaining = data.size();
  auto start = std::chrono::steady_clock::now();
  auto maxTs = start + totalTimeout;
  while (remaining > 0) {
    auto sent = ::send(fd, cursor, remaining, 0);
    if (sent <= 0) {
      log::error("sendAll failed with error {}", std::strerror(errno));
      if (std::chrono::steady_clock::now() >= maxTs) {
        log::error("sendAll timed out after {} ms", totalTimeout.count());
        return false;
      }
      std::this_thread::sleep_for(1ms);
      continue;
    }
    cursor += sent;
    remaining -= static_cast<std::size_t>(sent);
  }
  return true;
}

// Reads until we have a complete HTTP response or timeout.
// For chunked responses, continues reading until the terminating chunk (0\r\n\r\n).
// For responses with Content-Length, continues until body is complete.
// For Connection: close responses, reads until peer closes or timeout.
std::string recvWithTimeout(int fd, std::chrono::milliseconds totalTimeout) {
  std::string out;
  auto start = std::chrono::steady_clock::now();
  auto maxTs = start + totalTimeout;
  bool madeProgress = false;
  int consecutiveEagain = 0;

  while (std::chrono::steady_clock::now() < maxTs) {
    static constexpr std::size_t kChunkSize = static_cast<std::size_t>(64) * 1024ULL;
    std::size_t oldSize = out.size();

    if (out.capacity() < out.size() + kChunkSize) {
      // ensure exponential growth
      out.reserve(out.capacity() * 2UL);
    }

    out.resize_and_overwrite(oldSize + kChunkSize, [&](char *data, [[maybe_unused]] std::size_t newCap) {
      ssize_t recvBytes = ::recv(fd, data + oldSize, kChunkSize, MSG_DONTWAIT);
      if (recvBytes > 0) {
        return oldSize + static_cast<std::size_t>(recvBytes);
      }
      if (recvBytes == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        // No data currently; shrink back.
        return oldSize;  // no growth
      }
      // Closed or error.
      return oldSize;  // caller will break
    });

    if (out.size() > oldSize) {
      madeProgress = true;
      consecutiveEagain = 0;
      continue;  // Try to drain more immediately available bytes.
    }

    // Check if we have a complete HTTP response
    if (madeProgress && !out.empty()) {
      // Find header boundary
      auto headerEnd = out.find("\r\n\r\n");
      if (headerEnd != std::string::npos) {
        std::size_t bodyStart = headerEnd + 4;
        std::string_view headers(out.data(), headerEnd);

        // Check for chunked encoding
        auto tePos = headers.find("Transfer-Encoding: chunked");
        if (tePos == std::string_view::npos) {
          tePos = headers.find("transfer-encoding: chunked");
        }
        if (tePos != std::string_view::npos) {
          // For chunked: look for terminating "0\r\n\r\n"
          if (out.size() >= bodyStart + 5) {
            std::string_view body(out.data() + bodyStart, out.size() - bodyStart);
            if (body.find("0\r\n\r\n") != std::string_view::npos) {
              // Complete chunked response received
              break;
            }
          }
        } else {
          // Check for Content-Length
          auto clPos = headers.find("Content-Length:");
          if (clPos == std::string_view::npos) {
            clPos = headers.find("content-length:");
          }

          if (clPos != std::string_view::npos) {
            // Parse Content-Length value
            auto lineStart = clPos;
            auto lineEnd = headers.find("\r\n", lineStart);
            if (lineEnd != std::string_view::npos) {
              auto colonPos = headers.find(':', lineStart);
              if (colonPos != std::string_view::npos && colonPos < lineEnd) {
                auto valueStart = colonPos + 1;
                while (valueStart < lineEnd && headers[valueStart] == ' ') {
                  ++valueStart;
                }
                std::string_view lengthStr = headers.substr(valueStart, lineEnd - valueStart);
                std::size_t contentLength = 0;
                auto [ptr, ec] = std::from_chars(lengthStr.data(), lengthStr.data() + lengthStr.size(), contentLength);
                if (ec == std::errc{}) {
                  // Check if we have the full body
                  if (out.size() >= bodyStart + contentLength) {
                    break;
                  }
                }
              }
            }
          } else {
            // No Content-Length, no chunked - check for Connection: close
            // In this case, we need to wait for more data or connection close
            // Give it a few more attempts before breaking
            if (++consecutiveEagain > 10) {
              // Probably connection will close, or response is complete
              break;
            }
          }
        }
      }
    }

    std::this_thread::sleep_for(1ms);
  }
  return out;
}

std::string recvUntilClosed(int fd) {
  std::string out;
  for (;;) {
    static constexpr std::size_t kChunkSize = static_cast<std::size_t>(64) * 1024ULL;
    std::size_t oldSize = out.size();
    bool closed = false;

    if (out.capacity() < out.size() + kChunkSize) {
      // ensure exponential growth
      out.reserve(out.capacity() * 2UL);
    }

    out.resize_and_overwrite(oldSize + kChunkSize, [&](char *data, [[maybe_unused]] std::size_t newCap) {
      ssize_t recvBytes = ::recv(fd, data + oldSize, kChunkSize, 0);
      if (recvBytes > 0) {
        return oldSize + static_cast<std::size_t>(recvBytes);
      }
      closed = true;
      return oldSize;  // shrink back
    });
    if (closed) {
      break;
    }
  }
  return out;
}

std::string sendAndCollect(uint16_t port, std::string_view raw) {
  ClientConnection clientConnection(port);
  int fd = clientConnection.fd();

  sendAll(fd, raw);
  return recvUntilClosed(fd);
}

namespace {
void connectLoop(int fd, auto port, std::chrono::milliseconds timeout) {
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  for (const auto deadline = std::chrono::steady_clock::now() + timeout; std::chrono::steady_clock::now() < deadline;
       std::this_thread::sleep_for(std::chrono::milliseconds{1})) {
    if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0) {
      break;
    }
    log::debug("connect failed for fd={}: {}", fd, std::strerror(errno));
  }
}
}  // namespace

ClientConnection::ClientConnection(uint16_t port, std::chrono::milliseconds timeout) : _socket(SOCK_STREAM) {
  connectLoop(_socket.fd(), port, timeout);
}

int countOccurrences(std::string_view haystack, std::string_view needle) {
  if (needle.empty()) {
    return 0;
  }
  int count = 0;
  std::size_t pos = 0;
  while ((pos = haystack.find(needle, pos)) != std::string_view::npos) {
    ++count;
    pos += needle.size();
  }
  return count;
}

bool noBodyAfterHeaders(std::string_view raw) {
  const auto pivot = raw.find(aeronet::http::DoubleCRLF);
  if (pivot == std::string_view::npos) {
    return false;
  }
  return raw.substr(pivot + aeronet::http::DoubleCRLF.size()).empty();
}

// Very small blocking GET helper (Connection: close) used by tests that just need
// the full raw HTTP response bytes. Not HTTP-complete (no redirects, TLS, etc.).
std::string simpleGet(uint16_t port, std::string_view path) {
  ClientConnection cnx(port);
  if (cnx.fd() < 0) {
    return {};
  }
  std::string req;
  req.reserve(64 + path.size());
  req.append("GET ").append(path).append(" HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close");
  req.append(aeronet::http::DoubleCRLF);
  if (!sendAll(cnx.fd(), req)) {
    return {};
  }
  return recvUntilClosed(cnx.fd());
}

namespace {
std::string dechunk(std::string_view raw) {
  std::string out;
  size_t cursor = 0;
  while (cursor < raw.size()) {
    auto lineEnd = raw.find(http::CRLF, cursor);
    if (lineEnd == std::string::npos) {
      break;  // malformed
    }
    std::string_view sizeLine = raw.substr(cursor, lineEnd - cursor);
    cursor = lineEnd + http::CRLF.size();
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
    cursor += http::CRLF.size();
  }
  return out;  // best effort
}
}  // namespace

// Minimal GET request helper used across compression streaming tests. Parses headers into a map and returns body raw.
ParsedResponse simpleGet(uint16_t port, std::string_view target,
                         std::vector<std::pair<std::string, std::string>> extraHeaders) {
  RequestOptions opt;
  opt.target = std::string(target);
  opt.headers = std::move(extraHeaders);
  auto rawOpt = request(port, opt);
  if (!rawOpt) {
    throw std::runtime_error("request failed");
  }
  ParsedResponse out;
  const std::string &raw = *rawOpt;
  auto hEnd = raw.find(http::DoubleCRLF);
  if (hEnd == std::string::npos) {
    throw std::runtime_error("bad response");
  }
  out.headersRaw = raw.substr(0, hEnd + http::DoubleCRLF.size());
  auto statusLineEnd = out.headersRaw.find(http::CRLF);
  if (statusLineEnd != std::string::npos) {
    auto firstSpace = out.headersRaw.find(' ');
    if (firstSpace != std::string::npos) {
      auto secondSpace = out.headersRaw.find(' ', firstSpace + 1);
      auto codeStr = secondSpace == std::string::npos
                         ? out.headersRaw.substr(firstSpace + 1, statusLineEnd - firstSpace - 1)
                         : out.headersRaw.substr(firstSpace + 1, secondSpace - firstSpace - 1);
      try {
        out.statusCode = static_cast<aeronet::http::StatusCode>(read3(codeStr.data()));
      } catch (...) {
        out.statusCode = -1;
      }
    }
  }
  size_t cursor = 0;
  auto nextLine = [&](size_t &pos) {
    auto le = out.headersRaw.find(http::CRLF, pos);
    if (le == std::string::npos) {
      return std::string_view{};
    }
    std::string_view line(out.headersRaw.data() + pos, le - pos);
    pos = le + http::CRLF.size();
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
    std::size_t vs = colon + 1;
    while (vs < line.size() && line[vs] == ' ') {
      ++vs;
    }
    std::string val(line.substr(vs));
    out.headers.emplace(std::move(key), std::move(val));
  }
  out.body = raw.substr(hEnd + http::DoubleCRLF.size());
  // Derive plainBody (dechunk if necessary)
  auto te = out.headers.find("Transfer-Encoding");
  if (te != out.headers.end() && te->second == "chunked") {
    out.plainBody = dechunk(out.body);
  } else {
    out.plainBody = out.body;
  }
  return out;
}

std::string toLower(std::string input) {
  for (char &ch : input) {
    ch = ::aeronet::tolower(ch);
  }
  return input;
}

// Very small HTTP/1.1 response parser (not resilient to all malformed cases, just for test consumption)
std::optional<ParsedResponse> parseResponse(const std::string &raw) {
  ParsedResponse pr;
  std::size_t pos = raw.find(::aeronet::http::CRLF);
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
  pr.statusCode = static_cast<aeronet::http::StatusCode>(
      read3(statusLine.substr(firstSpace + 1, secondSpace - firstSpace - 1).data()));
  pr.reason = statusLine.substr(secondSpace + 1);
  std::size_t headerEnd = raw.find(::aeronet::http::CRLF, pos + ::aeronet::http::CRLF.size());
  if (headerEnd == std::string::npos) {
    return std::nullopt;
  }
  std::size_t cursor = pos + ::aeronet::http::CRLF.size();
  while (cursor < headerEnd) {
    std::size_t lineEnd = raw.find(::aeronet::http::CRLF, cursor);
    if (lineEnd == std::string::npos || lineEnd > headerEnd) {
      break;
    }
    std::string line = raw.substr(cursor, lineEnd - cursor);
    cursor = lineEnd + ::aeronet::http::CRLF.size();
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
    std::size_t lineEnd = bodyRaw.find(::aeronet::http::CRLF, bpos);
    if (lineEnd == std::string::npos) {
      break;
    }
    std::string lenHex = bodyRaw.substr(bpos, lineEnd - bpos);
    bpos = lineEnd + ::aeronet::http::CRLF.size();
    // lenHex may include chunk extensions (e.g. "4;ext=val"). Trim at first ';'.
    auto semipos = lenHex.find(';');
    std::string lenOnly = (semipos == std::string::npos) ? lenHex : lenHex.substr(0, semipos);
    // Parse hex using std::from_chars for robustness (no locale, fast).
    std::size_t chunkLen = 0;
    auto fc = std::from_chars(lenOnly.data(), lenOnly.data() + lenOnly.size(), chunkLen, 16);
    if (fc.ec != std::errc() || chunkLen == 0) {
      // Either parse error or terminating 0 chunk -> stop dechunking
      break;
    }
    if (bpos + chunkLen > bodyRaw.size()) {
      break;  // malformed / truncated
    }
    pr.body.append(bodyRaw.data() + bpos, chunkLen);
    bpos += chunkLen;
    // expect CRLF
    if (bpos + ::aeronet::http::CRLF.size() > bodyRaw.size()) {
      break;
    }
    bpos += ::aeronet::http::CRLF.size();  // skip CRLF
  }
  return pr;
}

bool setRecvTimeout(int fd, ::aeronet::Duration timeout) {
  const int timeoutMs = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(timeout).count());
  struct timeval tv{timeoutMs / 1000, static_cast<long>((timeoutMs % 1000) * 1000)};
  return ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0;
}

std::string buildRequest(const RequestOptions &opt) {
  std::string req;
  req.reserve(256 + opt.body.size());
  req.append(opt.method).append(" ").append(opt.target).append(" HTTP/1.1\r\n");
  req.append("Host: ").append(opt.host).append(::aeronet::http::CRLF);
  req.append("Connection: ").append(opt.connection).append(::aeronet::http::CRLF);
  for (auto &header : opt.headers) {
    req.append(header.first).append(::aeronet::http::HeaderSep).append(header.second).append(::aeronet::http::CRLF);
  }
  if (!opt.body.empty()) {
    req.append("Content-Length: ").append(std::to_string(opt.body.size())).append(::aeronet::http::CRLF);
  }
  req.append(::aeronet::http::CRLF);
  req.append(opt.body);
  return req;
}

std::optional<std::string> request(uint16_t port, const RequestOptions &opt) {
  ClientConnection cnx(port);
  int fd = cnx.fd();
  setRecvTimeout(fd, std::chrono::seconds(opt.recvTimeoutSeconds));
  auto reqStr = buildRequest(opt);
  ssize_t sent = ::send(fd, reqStr.data(), reqStr.size(), 0);
  if (std::cmp_not_equal(sent, reqStr.size())) {
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
      if (recvBytes < 0) {
        aeronet::log::error("request: recv error or connection closed, errno={}", std::strerror(errno));
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
std::string requestOrThrow(uint16_t port, const RequestOptions &opt) {
  auto resp = request(port, opt);
  if (!resp.has_value()) {
    throw std::runtime_error("requestOrThrow: request failed (socket/connect/send/recv) ");
  }
  return std::move(*resp);
}

// Send multiple requests over a single keep-alive connection and return raw responses individually.
// Limitations: assumes server responds fully before next request is parsed (sufficient for simple tests).
std::vector<std::string> sequentialRequests(uint16_t port, std::span<const RequestOptions> reqs) {
  std::vector<std::string> results;
  if (reqs.empty()) {
    return results;
  }
  ClientConnection cnx(port);
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
          aeronet::log::error("request: recv error or connection closed, errno={}", std::strerror(errno));
          return oldSize;  // timeout or close
        }

        return oldSize + static_cast<std::size_t>(recvBytes);
      });

      if (!headersDone) {
        auto hpos = out.find(::aeronet::http::DoubleCRLF);
        if (hpos != std::string::npos) {
          headersDone = true;
          // Parse minimal headers for length/chunked
          std::string headerBlock = out.substr(0, hpos + ::aeronet::http::DoubleCRLF.size());
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
              contentLen = StringToIntegral<decltype(contentLen)>(val);
            }
            if (keyLower == "transfer-encoding" && toLower(val).find("chunked") != std::string::npos) {
              chunked = true;
            }
          }
          if (chunked) {
            // For simplicity, read until terminating 0 chunk appears.
            if (out.find("\r\n0\r\n\r\n", hpos + ::aeronet::http::DoubleCRLF.size()) != std::string::npos) {
              break;
            }
          } else if (haveContentLen) {
            std::size_t bodySoFar = out.size() - (hpos + ::aeronet::http::DoubleCRLF.size());
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
          auto hpos = out.find(::aeronet::http::DoubleCRLF);
          if (hpos != std::string::npos) {
            std::size_t bodySoFar = out.size() - (hpos + ::aeronet::http::DoubleCRLF.size());
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

}  // namespace aeronet::test
