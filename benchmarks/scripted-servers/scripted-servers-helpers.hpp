#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <random>
#include <string>
#include <string_view>
#include <thread>

namespace bench {

// CPU-bound computation for /compute endpoint
constexpr uint64_t Fibonacci(int n) {
  if (n <= 1) {
    return static_cast<uint64_t>(n);
  }
  uint64_t prev = 0;
  uint64_t curr = 1;
  for (int i = 2; i <= n; ++i) {
    uint64_t next = prev + curr;
    prev = curr;
    curr = next;
  }
  return curr;
}

// Simple hash computation for CPU stress
constexpr uint64_t ComputeHash(std::string_view data, int iterations) {
  uint64_t hash = 0xcbf29ce484222325ULL;  // FNV-1a offset basis
  for (int iter = 0; iter < iterations; ++iter) {
    for (char signedCh : data) {
      auto ch = static_cast<unsigned char>(signedCh);
      hash ^= ch;
      hash *= 0x100000001b3ULL;  // FNV-1a prime
    }
  }
  return hash;
}

// Generate random string for response bodies
constexpr std::string GenerateRandomString(std::size_t length) {
  static constexpr char kCharset[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
  static thread_local std::mt19937_64 rng(std::random_device{}());
  std::uniform_int_distribution<std::size_t> dist(0, sizeof(kCharset) - 2);

  std::string result;
  result.resize_and_overwrite(length, [&dist](char* out, std::size_t n) {
    for (std::size_t pos = 0; pos < n; ++pos) {
      out[pos] = kCharset[dist(rng)];
    }
    return n;
  });
  return result;
}

inline int GetNumThreads() {
  const char* envThreads = std::getenv("BENCH_THREADS");
  if (envThreads != nullptr) {
    return std::atoi(envThreads);
  }
  int hwThreads = static_cast<int>(std::thread::hardware_concurrency());
  return std::max(1, hwThreads / 2);
}

inline std::string BuildJson(std::size_t itemCount) {
  std::string json = "{\"items\":[";
  for (std::size_t itemPos = 0; itemPos < itemCount; ++itemPos) {
    if (itemPos > 0) {
      json += ",";
    }
    json.append(R"({{"id":)");
    json += std::to_string(itemPos);
    json += R"(,"name":"item-)";
    json += std::to_string(itemPos);
    json += R"(","value":)";
    json += std::to_string(itemPos * 100);
    json += "}";
  }
  json += "]}";
  return json;
}

}  // namespace bench