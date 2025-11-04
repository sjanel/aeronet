// Benchmark utilities bridging a subset of the test utilities without
// forcing benchmarks to include the entire test header set.
// Intentionally tiny: only what internal microbenchmarks need.
#pragma once

#include <chrono>
#include <string>
#include <string_view>

#include "aeronet/test_util.hpp"

namespace bench_util {
using ClientConnection = ::aeronet::test::ClientConnection;  // re-export
using namespace std::chrono_literals;

inline void sendAll(int fd, std::string_view data) { ::aeronet::test::sendAll(fd, data); }
inline std::string recvWithTimeout(int fd, std::chrono::milliseconds total = 200ms) {
  return ::aeronet::test::recvWithTimeout(fd, total);
}
}  // namespace bench_util
