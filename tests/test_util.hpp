#pragma once

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

#include "base-fd.hpp"
#include "socket.hpp"

using namespace std::chrono_literals;

inline bool tu_sendAll(int fd, std::string_view data) {
  const char* cursor = data.data();
  std::size_t remaining = data.size();
  while (remaining > 0) {
    auto sent = ::send(fd, cursor, remaining, 0);
    if (sent <= 0) {
      return false;
    }
    cursor += sent;
    remaining -= static_cast<std::size_t>(sent);
  }
  return true;
}

inline std::string tu_recvWithTimeout(int fd, std::chrono::milliseconds totalTimeout = 200ms) {
  std::string out;
  char buffer[4096];
  auto start = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() - start < totalTimeout) {
    auto received = ::recv(fd, buffer, sizeof(buffer), MSG_DONTWAIT);
    if (received > 0) {
      out.append(buffer, buffer + received);
      continue;
    }
    if (received == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      std::this_thread::sleep_for(5ms);
      continue;
    }
    break;
  }
  return out;
}

inline std::string tu_recvUntilClosed(int fd) {
  std::string out;
  char buffer[4096];
  for (;;) {
    ssize_t received = ::recv(fd, buffer, sizeof(buffer), 0);
    if (received <= 0) {
      break;
    }
    out.append(buffer, buffer + received);
  }
  return out;
}

inline void tu_connect(int fd, auto port, std::chrono::milliseconds timeout = std::chrono::milliseconds{1000}) {
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  for (const auto deadline = std::chrono::steady_clock::now() + timeout; std::chrono::steady_clock::now() < deadline;
       std::this_thread::sleep_for(std::chrono::milliseconds{10})) {
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
      break;
    }
  }
}

class ClientConnection : public aeronet::Socket {
 public:
  ClientConnection() noexcept = default;

  ClientConnection(auto port, std::chrono::milliseconds timeout = std::chrono::milliseconds{1000})
      : Socket(aeronet::Socket::Type::STREAM) {
    tu_connect(fd(), port, timeout);
  }
};

// Count non-overlapping occurrences of a needle inside a haystack.
inline int tu_countOccurrences(std::string_view haystack, std::string_view needle) {
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

// Assert helper: verify no bytes after header terminator (used for HEAD behavior checks).
inline bool tu_noBodyAfterHeaders(std::string_view raw) {
  auto pivot = raw.find("\r\n\r\n");
  if (pivot == std::string_view::npos) {
    return false;
  }
  return raw.substr(pivot + 4).empty();
}