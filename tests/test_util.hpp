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

inline int tu_connect(auto port) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    throw std::runtime_error("Unable to open a new socket");
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  int err = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
  if (err != 0) {
    throw std::runtime_error("Unable to call ::connect");
  }
  return fd;
}

class ClientConnection : public aeronet::BaseFd {
 public:
  ClientConnection() noexcept = default;

  explicit ClientConnection(auto port) : BaseFd(tu_connect(port)) {
    if (!isOpened()) {
      throw std::runtime_error("Unable to open FD");
    }
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