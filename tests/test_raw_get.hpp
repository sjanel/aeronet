#pragma once

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "socket.hpp"

// Tiny shared raw GET helper for tests that still use ad-hoc sockets.
// Provides ASSERT-based hard failures instead of returning empty strings on errors.
// For richer scenarios (timeouts, multi-request, streaming) prefer test_http_client.hpp utilities.
namespace test_helpers {

inline void rawGet(uint16_t port, std::string_view path, std::string& out, std::string_view host = "127.0.0.1") {
  aeronet::Socket socket(aeronet::Socket::Type::STREAM);

  int fd = socket.fd();

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);

  ASSERT_EQ(::inet_pton(AF_INET, std::string(host).c_str(), &addr.sin_addr), 1) << "inet_pton failed";

  int connectRet = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));

  ASSERT_EQ(connectRet, 0) << "connect() failed: " << errno;

  std::string req = std::string("GET ") + std::string(path) + " HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n";
  auto sent = ::send(fd, req.data(), req.size(), 0);

  ASSERT_EQ(sent, static_cast<decltype(sent)>(req.size())) << "send() partial/failed";

  out.clear();
  // Use C++23 std::string::resize_and_overwrite to grow directly into the
  // destination buffer, avoiding an intermediate stack buffer + append copy.
  // We keep bounded growth (1MB cap) and read in 4KB chunks.
  constexpr std::size_t kChunk = 4096;
  constexpr std::size_t kCap = 1U << 20;  // 1MB safety cap

  out.reserve(kChunk);

  while (out.size() < kCap) {
    std::size_t oldSize = out.size();
    std::size_t remaining = kCap - oldSize;
    std::size_t grow = remaining < kChunk ? remaining : kChunk;
    bool reachedEnd = false;  // set when EOF or error encountered

    out.resize_and_overwrite(oldSize + grow,
                             [&](char* data, [[maybe_unused]] std::size_t newSize) noexcept -> std::size_t {
                               auto bytesRead = ::recv(fd, data + oldSize, grow, 0);
                               if (std::cmp_less_equal(bytesRead, 0)) {
                                 reachedEnd = true;  // EOF or error
                                 return oldSize;     // discard reserved growth
                               }
                               return oldSize + static_cast<std::size_t>(bytesRead);
                             });

    if (reachedEnd) {
      break;  // normal completion (server closed) or error (asserts earlier would have fired on setup)
    }
  }
}

}  // namespace test_helpers
