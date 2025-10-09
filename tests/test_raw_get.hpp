#pragma once

#include <gtest/gtest.h>
#include <sys/socket.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "aeronet/test_util.hpp"

// Tiny shared raw GET helper for tests that still use ad-hoc sockets.
// Provides ASSERT-based hard failures instead of returning empty strings on errors.
// For richer scenarios (timeouts, multi-request, streaming) prefer test_http_client.hpp utilities.
namespace test_helpers {

inline void rawGet(uint16_t port, std::string_view path, std::string& out) {
  aeronet::test::ClientConnection cnx(port);

  int fd = cnx.fd();

  std::string req = std::string("GET ") + std::string(path) + " HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n";
  auto sent = ::send(fd, req.data(), req.size(), 0);

  ASSERT_EQ(sent, static_cast<decltype(sent)>(req.size())) << "send() partial/failed";

  out.clear();
  static constexpr std::size_t kChunkSize = 4096;
  static constexpr std::size_t kCap = 1U << 20;  // 1MB safety cap

  out.reserve(kChunkSize);

  while (out.size() < kCap) {
    std::size_t oldSize = out.size();
    std::size_t remaining = kCap - oldSize;
    std::size_t grow = remaining < kChunkSize ? remaining : kChunkSize;
    bool reachedEnd = false;  // set when EOF or error encountered

    if (out.capacity() < out.size() + kChunkSize) {
      // ensure exponential growth
      out.reserve(out.capacity() * 2UL);
    }

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
