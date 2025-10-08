#pragma once

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <thread>

#include "aeronet/http-constants.hpp"
#include "log.hpp"
#include "socket.hpp"

namespace aeronet::test {
using namespace std::chrono_literals;

inline bool sendAll(int fd, std::string_view data, std::chrono::milliseconds totalTimeout = 500ms) {
  const char* cursor = data.data();
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

inline std::string recvWithTimeout(int fd, std::chrono::milliseconds totalTimeout = 1000ms) {
  // Reads as much as is immediately available. After at least one successful read, a single EAGAIN ends the read early.
  // This lets callers loop without incurring the full timeout on keep-alive connections.
  std::string out;
  auto start = std::chrono::steady_clock::now();
  auto maxTs = start + totalTimeout;
  bool madeProgress = false;
  while (std::chrono::steady_clock::now() < maxTs) {
    constexpr std::size_t kChunk = static_cast<std::size_t>(64) * 1024ULL;
    std::size_t oldSize = out.size();
    // Use resize_and_overwrite; we only write up to kChunk bytes.
    std::size_t written = 0;
    out.resize_and_overwrite(oldSize + kChunk, [&](char* data, [[maybe_unused]] std::size_t newCap) {
      ssize_t recvBytes = ::recv(fd, data + oldSize, kChunk, MSG_DONTWAIT);
      if (recvBytes > 0) {
        written = static_cast<std::size_t>(recvBytes);
        return oldSize + written;
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
      continue;  // Try to drain more immediately available bytes.
    }
    // EAGAIN path (no growth) when progress already made -> break early
    if (madeProgress) {
      break;
    }
    std::this_thread::sleep_for(1ms);
  }
  return out;
}

inline std::string recvUntilClosed(int fd) {
  // Read until peer closes. Avoid temporary buffer by growing string directly.
  std::string out;
  for (;;) {
    constexpr std::size_t kChunk = static_cast<std::size_t>(64) * 1024ULL;
    std::size_t oldSize = out.size();
    std::size_t written = 0;
    bool closed = false;
    out.resize_and_overwrite(oldSize + kChunk, [&](char* data, [[maybe_unused]] std::size_t newCap) {
      ssize_t recvBytes = ::recv(fd, data + oldSize, kChunk, 0);
      if (recvBytes > 0) {
        written = static_cast<std::size_t>(recvBytes);
        return oldSize + written;
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

inline void connectLoop(int fd, auto port, std::chrono::milliseconds timeout = std::chrono::milliseconds{1000}) {
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  for (const auto deadline = std::chrono::steady_clock::now() + timeout; std::chrono::steady_clock::now() < deadline;
       std::this_thread::sleep_for(std::chrono::milliseconds{10})) {
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
      break;
    }
    log::debug("connect failed for fd={}: {}", fd, std::strerror(errno));
  }
}

class ClientConnection {
 public:
  ClientConnection() noexcept = default;

  explicit ClientConnection(auto port, std::chrono::milliseconds timeout = std::chrono::milliseconds{1000})
      : _socket(SOCK_STREAM) {
    connectLoop(_socket.fd(), port, timeout);
  }

  [[nodiscard]] int fd() const noexcept { return _socket.fd(); }

 private:
  ::aeronet::Socket _socket;
};

inline int countOccurrences(std::string_view haystack, std::string_view needle) {
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

inline bool noBodyAfterHeaders(std::string_view raw) {
  const auto pivot = raw.find(aeronet::http::DoubleCRLF);
  if (pivot == std::string_view::npos) {
    return false;
  }
  return raw.substr(pivot + aeronet::http::DoubleCRLF.size()).empty();
}

// Very small blocking GET helper (Connection: close) used by tests that just need
// the full raw HTTP response bytes. Not HTTP-complete (no redirects, TLS, etc.).
inline std::string simpleGet(uint16_t port, std::string_view path) {
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

}  // namespace aeronet::test
