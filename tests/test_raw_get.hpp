#pragma once
#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <string>
#include <string_view>

// Tiny shared raw GET helper for tests that still use ad-hoc sockets.
// Provides ASSERT-based hard failures instead of returning empty strings on errors.
// For richer scenarios (timeouts, multi-request, streaming) prefer test_http_client.hpp utilities.
namespace test_helpers {

inline void rawGet(uint16_t port, std::string_view path, std::string& out, std::string_view host = "127.0.0.1") {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(fd, 0) << "socket() failed: " << errno;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  ASSERT_EQ(::inet_pton(AF_INET, std::string(host).c_str(), &addr.sin_addr), 1) << "inet_pton failed";
  int connectRet = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
  ASSERT_EQ(connectRet, 0) << "connect() failed: " << errno;
  std::string req = std::string("GET ") + std::string(path) + " HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n";
  ssize_t sent = ::send(fd, req.data(), req.size(), 0);
  ASSERT_EQ(sent, static_cast<ssize_t>(req.size())) << "send() partial/failed";
  out.clear();
  char buf[4096];
  while (true) {
    ssize_t bytesRead = ::recv(fd, buf, sizeof(buf), 0);
    if (bytesRead <= 0) {
      break;
    }
    out.append(buf, buf + bytesRead);
    if (out.size() > (1U << 20)) {  // 1MB safety cap
      break;
    }
  }
  ::close(fd);
}

}  // namespace test_helpers
