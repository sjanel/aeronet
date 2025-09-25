// bench_util.hpp - shared helper utilities for Aeronet benchmarks
#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

#include "test_util.hpp"  // provides ClientConnection and helpers

namespace benchutil {

// A simple reusable persistent connection for issuing GET /data?size=N requests
// and reading the full response body. Returns body size or std::nullopt on error.
inline std::optional<std::size_t> requestBodySize(ClientConnection &conn, std::size_t n) {
  std::string req =
      "GET /data?size=" + std::to_string(n) + " HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n";
  if (!tu_sendAll(conn.fd(), req)) {
    return std::nullopt;
  }
  // We read with timeout so multiple pipelined requests can accumulate; we expect server to respond quickly.
  // For simplicity reuse the existing util (short timeout) then parse last response in buffer.
  auto raw = tu_recvWithTimeout(conn.fd(), std::chrono::milliseconds(50));
  auto pivot = raw.rfind("\r\n\r\n");
  if (pivot == std::string::npos) {
    return std::nullopt;
  }
  std::string_view body = std::string_view(raw).substr(pivot + 4);
  return body.size();
}

}  // namespace benchutil
