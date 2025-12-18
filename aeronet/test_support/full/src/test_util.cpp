#include "aeronet/test_util.hpp"

#include <asm-generic/socket.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>  // NOLINT(misc-include-cleaner) used by timeval
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <future>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "aeronet/base-fd.hpp"
#include "aeronet/errno-throw.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/log.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/simple-charconv.hpp"
#include "aeronet/socket.hpp"
#include "aeronet/timedef.hpp"
#include "aeronet/toupperlower.hpp"
#ifdef AERONET_ENABLE_BROTLI
#include "aeronet/brotli-decoder.hpp"
#endif
#ifdef AERONET_ENABLE_ZLIB
#include "aeronet/zlib-decoder.hpp"
#endif
#ifdef AERONET_ENABLE_ZSTD
#include "aeronet/zstd-decoder.hpp"
#endif

namespace aeronet::test {
using namespace std::chrono_literals;

namespace {
constexpr std::size_t kChunkSize = 1 << 13;
}

void sendAll(int fd, std::string_view data, std::chrono::milliseconds totalTimeout) {
  const char *cursor = data.data();
  const auto start = std::chrono::steady_clock::now();
  const auto maxTs = start + totalTimeout;

  bool alreadyLoggedError = false;

  for (std::size_t remaining = data.size(); remaining > 0;) {
    const auto sent = ::send(fd, cursor, remaining, MSG_NOSIGNAL);
    if (sent <= 0) {
      const auto err = errno;
      if (!alreadyLoggedError) {
        alreadyLoggedError = true;
        log::error("sendAll failed with error {}", std::strerror(err));
      }
      if (std::chrono::steady_clock::now() >= maxTs) {
        log::error("sendAll timed out after {} ms", totalTimeout.count());
        throw std::runtime_error("sendAll timed out");
      }
      std::this_thread::sleep_for(1ms);  // NOLINT(misc-include-cleaner) include chrono is there
      continue;
    }
    cursor += sent;
    remaining -= static_cast<std::size_t>(sent);
  }
}

std::string recvWithTimeout(int fd, std::chrono::milliseconds totalTimeout) {
  std::string out;
  auto start = std::chrono::steady_clock::now();
  auto maxTs = start + totalTimeout;

  while (std::chrono::steady_clock::now() < maxTs) {
    const std::size_t oldSize = out.size();

    if (out.capacity() < out.size() + kChunkSize) {
      const auto desired = out.size() + kChunkSize;
      const auto doubled = (out.capacity() > 0) ? (out.capacity() * 2UL) : desired;
      out.reserve(std::max(doubled, desired));
    }

    bool again = false;

    out.resize_and_overwrite(oldSize + kChunkSize,
                             [fd, oldSize, &again](char *data, [[maybe_unused]] std::size_t newCap) {
                               ssize_t recvBytes = ::recv(fd, data + oldSize, kChunkSize, MSG_DONTWAIT);
                               if (recvBytes == -1) {
                                 if (errno == EAGAIN || errno == EWOULDBLOCK) {
                                   std::this_thread::sleep_for(std::chrono::milliseconds{1});
                                   again = true;
                                   return oldSize;
                                 }
                                 throw_errno("Error from non-blocking recv");
                               }
                               return oldSize + static_cast<std::size_t>(recvBytes);
                             });

    if (again) {
      continue;
    }

    auto headerEnd = out.find(http::DoubleCRLF);
    if (headerEnd != std::string::npos) {
      std::size_t bodyStart = headerEnd + http::DoubleCRLF.size();
      std::string_view headers(out.data(), headerEnd);

      auto tePos = headers.find("Transfer-Encoding: chunked");
      if (tePos == std::string_view::npos) {
        tePos = headers.find("transfer-encoding: chunked");
      }
      if (tePos != std::string_view::npos) {
        if (out.size() >= bodyStart + 5) {
          std::string_view body(out.data() + bodyStart, out.size() - bodyStart);
          if (body.find("0\r\n\r\n") != std::string_view::npos) {
            break;
          }
        }
      } else {
        auto clPos = headers.find("Content-Length:");
        if (clPos == std::string_view::npos) {
          clPos = headers.find("content-length:");
        }
        if (clPos != std::string_view::npos) {
          auto lineStart = clPos;
          auto lineEnd = headers.find(http::CRLF, lineStart);
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
                if (out.size() >= bodyStart + contentLength) {
                  break;
                }
              }
            }
          }
        }
      }
      if (out.size() == oldSize) {
        break;
      }
    }
  }

  return out;
}

std::string recvUntilClosed(int fd) {
  std::string out;
  for (;;) {
    const std::size_t oldSize = out.size();

    if (out.capacity() < out.size() + kChunkSize) {
      out.reserve((out.capacity() * 2UL) + kChunkSize);
    }

    out.resize_and_overwrite(oldSize + kChunkSize, [fd, oldSize](char *data, [[maybe_unused]] std::size_t newCap) {
      const ssize_t recvBytes = ::recv(fd, data + oldSize, kChunkSize, 0);
      if (recvBytes == -1) {
        // When SO_RCVTIMEO is set on a blocking socket, recv returns -1 with errno = EAGAIN/EWOULDBLOCK
        // when the timeout expires.
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          std::this_thread::sleep_for(1ms);
          return oldSize;
        }
        throw_errno("Error from blocking recv");
      }

      return oldSize + static_cast<std::size_t>(recvBytes);
    });

    if (out.size() == oldSize) {
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

std::pair<Socket, uint16_t> startEchoServer() {
  Socket listenSock(Socket::Type::Stream);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;  // ephemeral
  if (::bind(listenSock.fd(), reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
    throw_errno("Error from ::bind");
  }
  if (::listen(listenSock.fd(), 1) == -1) {
    throw_errno("Error from ::listen");
  }
  sockaddr_in actual{};
  socklen_t alen = sizeof(actual);
  if (::getsockname(listenSock.fd(), reinterpret_cast<sockaddr *>(&actual), &alen) != 0) {
    throw_errno("Error from ::getsockname");
  }

  uint16_t port = ntohs(actual.sin_port);

  std::thread([fd = listenSock.fd()]() {
    BaseFd clientFd(::accept(fd, nullptr, nullptr));
    if (clientFd.fd() == -1) {
      throw_errno("Error from ::accept");
    }
    char buf[1024];

    while (true) {
      ssize_t recvBytes = ::recv(clientFd.fd(), buf, static_cast<int>(sizeof(buf)), 0);
      if (recvBytes == -1) {
        throw_errno("Error from ::recv");
      }
      if (recvBytes == 0) {
        break;  // connection closed
      }

      try {
        sendAll(clientFd.fd(), std::string_view(buf, static_cast<std::size_t>(recvBytes)));
      } catch (const std::exception &ex) {
        log::error("Echo server sendAll failed: {}", ex.what());
        break;
      }
    }
  }).detach();

  return std::make_pair(std::move(listenSock), port);
}

namespace {
Socket connectLoop(auto port, std::chrono::milliseconds timeout) {
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  for (const auto deadline = std::chrono::steady_clock::now() + timeout; std::chrono::steady_clock::now() < deadline;
       std::this_thread::sleep_for(std::chrono::milliseconds{1})) {
    Socket sock(Socket::Type::Stream);
    int fd = sock.fd();

    if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0) {
      return sock;
    }

    log::debug("connect failed for fd # {}: {}", fd, std::strerror(errno));
  }
  log::error("Error from ::connect for: {}", std::strerror(errno));
  return Socket{};
}
}  // namespace

ClientConnection::ClientConnection(uint16_t port, std::chrono::milliseconds timeout)
    : _socket(connectLoop(port, timeout)) {}

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
  const auto pivot = raw.find(http::DoubleCRLF);
  if (pivot == std::string_view::npos) {
    return false;
  }
  return raw.substr(pivot + http::DoubleCRLF.size()).empty();
}

std::string simpleGet(uint16_t port, std::string_view path) {
  ClientConnection cnx(port);
  if (cnx.fd() == -1) {
    throw std::runtime_error("simpleGet: failed to connect");
  }
  std::string req;
  req.reserve(64 + path.size());
  req.append("GET ").append(path).append(" HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close");
  req.append(http::DoubleCRLF);
  sendAll(cnx.fd(), req);
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
        out.statusCode = static_cast<http::StatusCode>(read3(codeStr.data()));
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
  nextLine(cursor);
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
    ch = tolower(ch);
  }
  return input;
}

// Very small HTTP/1.1 response parser (not resilient to all malformed cases, just for test consumption)
std::optional<ParsedResponse> parseResponse(std::string_view raw) {
  ParsedResponse pr;
  std::size_t pos = raw.find(http::CRLF);
  if (pos == std::string_view::npos) {
    return std::nullopt;
  }
  std::string_view statusLine = raw.substr(0, pos);
  // Expect: HTTP/1.1 <code> <reason>
  auto firstSpace = statusLine.find(' ');
  if (firstSpace == std::string_view::npos) {
    return std::nullopt;
  }
  auto codeEnd = statusLine.find(' ', firstSpace + 1);
  if (codeEnd == std::string_view::npos) {
    codeEnd = statusLine.size();
    pr.reason.clear();
  } else {
    pr.reason = statusLine.substr(codeEnd + 1);
  }
  pr.statusCode = static_cast<http::StatusCode>(read3(statusLine.data() + firstSpace + 1));
  std::size_t headerEnd = raw.find(http::DoubleCRLF, pos + http::CRLF.size());
  if (headerEnd == std::string_view::npos) {
    return std::nullopt;
  }
  pr.headersRaw = raw.substr(0, headerEnd + http::DoubleCRLF.size());
  std::size_t cursor = pos + http::CRLF.size();
  while (cursor < headerEnd) {
    std::size_t lineEnd = raw.find(http::CRLF, cursor);
    if (lineEnd == std::string_view::npos || lineEnd > headerEnd) {
      break;
    }
    std::string_view line = raw.substr(cursor, lineEnd - cursor);
    cursor = lineEnd + http::CRLF.size();
    if (line.empty()) {
      break;
    }
    auto colon = line.find(':');
    if (colon == std::string_view::npos) {
      continue;
    }
    std::string_view key = line.substr(0, colon);
    // skip space after colon if present
    std::size_t valueStart = colon + 1;
    if (valueStart < line.size() && line[valueStart] == ' ') {
      ++valueStart;
    }
    std::string_view value = line.substr(valueStart);
    pr.headers.insert_or_assign(std::string(key), std::string(value));
  }
  pr.chunked = false;
  auto teIt = pr.headers.find("Transfer-Encoding");
  if (teIt != pr.headers.end() && toLower(teIt->second).contains("chunked")) {
    pr.chunked = true;
  }
  std::string_view bodyRaw = raw.substr(headerEnd + http::DoubleCRLF.size());
  if (!pr.chunked) {
    pr.body = std::move(bodyRaw);
    pr.plainBody = pr.body;
    return pr;
  }
  // De-chunk (simple algorithm; ignores trailers)
  std::size_t bpos = 0;
  while (bpos < bodyRaw.size()) {
    std::size_t lineEnd = bodyRaw.find(http::CRLF, bpos);
    if (lineEnd == std::string_view::npos) {
      break;
    }
    std::string_view lenHex = bodyRaw.substr(bpos, lineEnd - bpos);
    bpos = lineEnd + http::CRLF.size();
    // lenHex may include chunk extensions (e.g. "4;ext=val"). Trim at first ';'.
    auto semipos = lenHex.find(';');
    std::string_view lenOnly = (semipos == std::string_view::npos) ? lenHex : lenHex.substr(0, semipos);
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
    if (bpos + http::CRLF.size() > bodyRaw.size()) {
      break;
    }
    bpos += http::CRLF.size();  // skip CRLF
  }
  pr.plainBody = pr.body;
  return pr;
}

ParsedResponse parseResponseOrThrow(std::string_view raw) {
  auto prOpt = parseResponse(raw);
  if (!prOpt) {
    throw std::runtime_error("parseResponse: failed to parse response");
  }
  return *prOpt;
}

void setRecvTimeout(int fd, SysDuration timeout) {
  const int timeoutMs = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(timeout).count());
  // NOLINTNEXTLINE(misc-include-cleaner) from <sys/time.h>
  struct timeval tv{timeoutMs / 1000, static_cast<long>((timeoutMs % 1000) * 1000)};
  if (::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == -1) {
    throw_errno("Error from setRecvTimeout");
  }
}

std::string buildRequest(const RequestOptions &opt) {
  std::string req;
  req.reserve(256 + opt.body.size());
  req.append(opt.method).append(" ").append(opt.target).append(" HTTP/1.1\r\n");
  req.append("Host: ").append(opt.host).append(http::CRLF);
  req.append("Connection: ").append(opt.connection).append(http::CRLF);
  for (auto &header : opt.headers) {
    req.append(header.first).append(http::HeaderSep).append(header.second).append(http::CRLF);
  }
  if (!opt.body.empty()) {
    bool haveCL = false;
    for (auto &header : opt.headers) {
      if (header.first.size() == sizeof("Content-Length") - 1 &&
          std::equal(header.first.begin(), header.first.end(), "Content-Length",
                     [](char chA, char chB) { return aeronet::tolower(chA) == aeronet::tolower(chB); })) {
        haveCL = true;
        break;
      }
    }
    if (!haveCL) {
      req.append("Content-Length: ").append(std::to_string(opt.body.size())).append(http::CRLF);
    }
  }
  req.append(http::CRLF);
  req.append(opt.body);
  return req;
}

std::optional<std::string> request(uint16_t port, const RequestOptions &opt) {
  ClientConnection cnx(port);
  int fd = cnx.fd();
  setRecvTimeout(fd, std::chrono::seconds(opt.recvTimeoutSeconds));
  auto reqStr = buildRequest(opt);

  sendAll(fd, reqStr);

  return recvUntilClosed(fd);
}

EncodingAndBody extractContentEncodingAndBody(std::string_view raw) {
  EncodingAndBody out;
  // Find header/body separator
  auto sep = raw.find(http::DoubleCRLF);
  if (sep == std::string_view::npos) {
    // No headers found: treat entire raw as body
    out.body.assign(raw);
    out.contentEncoding = std::string_view{};
    return out;
  }
  std::string_view headers = raw.substr(0, sep);
  std::string_view body = raw.substr(sep + http::DoubleCRLF.size());

  // Find Content-Encoding header (case-insensitive search for the header name)
  // We'll do a simple search for "Content-Encoding:" / "content-encoding:" and then extract the value.
  auto pos = headers.find("Content-Encoding:");
  if (pos == std::string_view::npos) {
    pos = headers.find("content-encoding:");
  }
  if (pos != std::string_view::npos) {
    // Find line end
    auto lineEnd = headers.find('\r', pos);
    if (lineEnd == std::string_view::npos) {
      lineEnd = headers.size();
    }
    auto valueStart = headers.find_first_not_of(" \t", pos + sizeof("Content-Encoding:") - 1);
    if (valueStart != std::string_view::npos && valueStart < lineEnd) {
      out.contentEncoding = headers.substr(valueStart, lineEnd - valueStart);
    }
  }

  // De-chunk if needed (look for Transfer-Encoding: chunked header)
  bool isChunked = false;
  auto tePos = headers.find("Transfer-Encoding:");
  if (tePos == std::string_view::npos) {
    tePos = headers.find("transfer-encoding:");
  }
  if (tePos != std::string_view::npos) {
    // crude check for "chunked" substring on the same header line
    auto lineEnd = headers.find('\r', tePos);
    std::string_view val =
        (lineEnd == std::string_view::npos) ? headers.substr(tePos) : headers.substr(tePos, lineEnd - tePos);
    if (val.find("chunked") != std::string_view::npos || val.find("Chunked") != std::string_view::npos) {
      isChunked = true;
    }
  }

  if (isChunked) {
    out.body = dechunk(body);
  } else {
    out.body.assign(body);
  }

  // If a content-encoding was present, try to decompress the body now so callers
  // receive the uncompressed payload. We do best-effort decompression and fall
  // back to the original bytes on failure.
  if (!out.contentEncoding.empty()) {
    std::string encLower = toLower(std::string(out.contentEncoding));

#ifdef AERONET_ENABLE_ZLIB
    if (encLower.contains("gzip") || encLower.contains("deflate")) {
      RawChars tmp;
      if (ZlibDecoder::Decompress(out.body, /*isGzip=*/encLower.contains("gzip"),
                                  /*maxDecompressedBytes=*/(1 << 20), /*decoderChunkSize=*/65536, tmp)) {
        out.body.assign(tmp.data(), tmp.data() + tmp.size());
        return out;
      }
    }
#endif

#ifdef AERONET_ENABLE_ZSTD
    if (encLower.contains("zstd")) {
      RawChars tmp;
      if (ZstdDecoder::Decompress(out.body, /*maxDecompressedBytes=*/(1 << 20),
                                  /*decoderChunkSize=*/65536, tmp)) {
        out.body.assign(tmp.data(), tmp.data() + tmp.size());
        return out;
      }
    }
#endif

#ifdef AERONET_ENABLE_BROTLI
    if (encLower.contains("br") || encLower.contains("brotli")) {
      RawChars tmp;
      if (BrotliDecoder::Decompress(out.body, /*maxDecompressedBytes=*/(1 << 20),
                                    /*decoderChunkSize=*/65536, tmp)) {
        out.body.assign(tmp.data(), tmp.data() + tmp.size());
        return out;
      }
    }
#endif
    throw std::invalid_argument("Unknown content encoding");
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

bool AttemptConnect(uint16_t port) {
  Socket sock(Socket::Type::Stream);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  return ::connect(sock.fd(), reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0;
}

bool WaitForPeerClose(int fd, std::chrono::milliseconds timeout) {
  // Use recvUntilClosed via an async future so we can time out waiting for remote close.
  auto fut = std::async(std::launch::async, [fd]() { return recvUntilClosed(fd); });
  if (fut.wait_for(timeout) == std::future_status::ready) {
    // peer closed and we received final data
    fut.get();
    return true;
  }
  return false;
}

bool WaitForListenerClosed(uint16_t port, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (!AttemptConnect(port)) {
      log::info("Confirmed listener on port {} is closed", port);
      return true;
    }
    std::this_thread::sleep_for(5ms);  // NOLINT(misc-include-cleaner) include chrono is there
  }
  return !AttemptConnect(port);
}

}  // namespace aeronet::test
