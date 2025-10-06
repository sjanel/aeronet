#pragma once

#include <sys/socket.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <optional>
#include <random>
#include <string>
#include <thread>

#include "aeronet/http-constants.hpp"
#include "aeronet/test_util.hpp"
#include "log.hpp"
#include "raw-chars.hpp"

namespace benchutil {

inline constexpr std::string kBodyPath = "/body";
inline constexpr std::string kHeaderPath = "/headers";

// Simplified blocking implementation: issue request, block until headers, then read body per Content-Length.
// This intentionally forgoes adaptive/non-blocking complexity to reduce flakiness.
inline std::optional<std::size_t> requestBodySize(std::string_view method, std::string_view path, int fd,
                                                  std::size_t requestedSize) {
  std::string req(method);
  req.push_back(' ');
  req.append(path);
  req.append("?size=");
  req.append(std::to_string(requestedSize));
  req.append(" HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: keep-alive\r\n\r\n");
  if (!aeronet::test::sendAll(fd, req)) {
    aeronet::log::error("sendAll failed: {}", std::strerror(errno));
    return std::nullopt;
  }

  constexpr auto kGlobalDeadline = std::chrono::seconds(15);
  const auto deadline = std::chrono::steady_clock::now() + kGlobalDeadline;
  aeronet::RawChars buffer;
  std::size_t contentLength = 0;
  bool haveCL = false;
  // 1. Read headers
  while (std::chrono::steady_clock::now() < deadline) {
    constexpr std::size_t kChunk = static_cast<std::size_t>(64) * 1024ULL;
    std::size_t oldSize = buffer.size();
    std::size_t written = 0;
    buffer.resize_and_overwrite(oldSize + kChunk, [&](char* data, [[maybe_unused]] std::size_t newCap) {
      for (;;) {
        ssize_t recvBytes = ::recv(fd, data + oldSize, kChunk, 0);  // blocking
        if (recvBytes > 0) {
          written = static_cast<std::size_t>(recvBytes);
          return oldSize + written;
        }
        if (recvBytes == -1 && errno == EINTR) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
          continue;  // retry
        }
        aeronet::log::debug("connection closed before headers: {}", std::strerror(errno));
        return oldSize;  // no growth, indicates close / other error
      }
    });
    if (buffer.size() == oldSize) {  // no progress
      return std::nullopt;
    }
    auto crlrRg = std::ranges::search(buffer, aeronet::http::DoubleCRLF);
    if (crlrRg.empty()) {
      continue;
    }
    std::string_view headers(buffer.data(), crlrRg.data() + aeronet::http::DoubleCRLF.size());
    auto clPos = headers.find("Content-Length:");
    if (clPos == std::string_view::npos) {
      clPos = headers.find("content-length:");
    }
    if (clPos == std::string_view::npos) {
      aeronet::log::error("Content-Length missing");
      return std::nullopt;
    }
    clPos += sizeof("Content-Length:") - 1;
    while (clPos < headers.size() && (headers[clPos] == ' ' || headers[clPos] == '\t')) {
      ++clPos;
    }
    while (clPos < headers.size() && headers[clPos] >= '0' && headers[clPos] <= '9') {
      contentLength = (contentLength * 10) + static_cast<std::size_t>(headers[clPos] - '0');
      haveCL = true;
      ++clPos;
      if (contentLength > (1U << 30)) {
        break;  // cap 1GB
      }
    }
    if (!haveCL) {
      aeronet::log::error("failed to parse Content-Length");
      return std::nullopt;
    }
    std::size_t bodyOffset = static_cast<std::size_t>(crlrRg.data() - buffer.data()) + aeronet::http::DoubleCRLF.size();
    std::size_t haveBody = buffer.size() - bodyOffset;
    if (contentLength == 0) {
      return 0U;
    }
    if (haveBody >= contentLength) {
      return contentLength;
    }
    // 2. Read remaining body
    std::size_t remaining = contentLength - haveBody;
    while (remaining > 0 && std::chrono::steady_clock::now() < deadline) {
      std::size_t chunkSize = std::min<std::size_t>(static_cast<std::size_t>(64) * 1024ULL, remaining);
      std::size_t oldBodySize = buffer.size();
      std::size_t bodyWritten = 0;
      buffer.resize_and_overwrite(oldBodySize + chunkSize, [&](char* data, std::size_t /*nc*/) {
        for (;;) {
          ssize_t recvBytes = ::recv(fd, data + oldBodySize, chunkSize, 0);
          if (recvBytes > 0) {
            bodyWritten = static_cast<std::size_t>(recvBytes);
            return oldBodySize + bodyWritten;
          }
          if (recvBytes == -1 && errno == EINTR) {
            continue;  // retry
          }
          return oldBodySize;  // rollback
        }
      });
      if (bodyWritten == 0) {
        aeronet::log::error("body truncated: wanted {}, have {}", contentLength, contentLength - remaining);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      remaining -= bodyWritten;
    }
    if (remaining == 0) {
      return contentLength;
    }
    aeronet::log::error("timeout reading body: wanted {}, missing {}", contentLength, remaining);
    return std::nullopt;
  }
  aeronet::log::error("timeout waiting for headers ({}s cap)", kGlobalDeadline.count());
  return std::nullopt;
}

inline std::string randomStr(std::size_t n, std::mt19937_64& rng) {
  std::string out(n, 'X');
  static constexpr char charset[] =
      "0123456789"
      "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
      "abcdefghijklmnopqrstuvwxyz";
  std::uniform_int_distribution<std::size_t> dist(0, std::size(charset) - 2);
  for (std::size_t charPos = 1; charPos < out.size(); ++charPos) {
    out[charPos] = charset[dist(rng)];
  }

  return out;
}

}  // namespace benchutil
