#pragma once
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

using namespace std::chrono_literals;

inline int tu_connect(uint16_t port) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

inline bool tu_sendAll(int fd, const std::string& data) {
  const char* cursor = data.data();
  size_t remaining = data.size();
  while (remaining > 0) {
    ssize_t sent = ::send(fd, cursor, remaining, 0);
    if (sent <= 0) {
      return false;
    }
    cursor += sent;
    remaining -= static_cast<size_t>(sent);
  }
  return true;
}

inline std::string tu_recvWithTimeout(int fd, std::chrono::milliseconds totalTimeout = 200ms) {
  std::string out;
  char buffer[4096];
  auto start = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() - start < totalTimeout) {
    ssize_t received = ::recv(fd, buffer, sizeof(buffer), MSG_DONTWAIT);
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
