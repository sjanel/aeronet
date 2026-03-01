#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <thread>

#include "aeronet/zlib-gateway.hpp"

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

inline bool ContainsTokenInsensitive(std::string_view haystack, std::string_view needle) {
  if (needle.empty()) {
    return false;
  }
  auto lower = [](unsigned char ch) { return static_cast<unsigned char>(std::tolower(ch)); };
  for (std::size_t pos = 0; pos + needle.size() <= haystack.size(); ++pos) {
    bool match = true;
    for (std::size_t idx = 0; idx < needle.size(); ++idx) {
      if (lower(static_cast<unsigned char>(haystack[pos + idx])) != lower(static_cast<unsigned char>(needle[idx]))) {
        match = false;
        break;
      }
    }
    if (match) {
      return true;
    }
  }
  return false;
}

inline std::optional<std::string> GzipCompress(std::string_view input, int level = Z_DEFAULT_COMPRESSION) {
  using namespace aeronet;
  zstream stream{};
  if (ZDeflateInit2(stream, level, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
    return std::nullopt;
  }

  ZSetInput(stream, input);

  std::string output;
  output.resize(ZDeflateBound(&stream, input.size()));

  ZSetOutput(stream, output.data(), output.size());

  int ret = ZDeflate(stream, Z_FINISH);
  if (ret != Z_STREAM_END) {
    ZDeflateEnd(stream);
    return std::nullopt;
  }

  output.resize(stream.total_out);
  ZDeflateEnd(stream);
  return output;
}

inline std::optional<std::string> GzipDecompress(std::string_view input) {
  using namespace aeronet;
  zstream stream{};
  if (ZInflateInit2(stream, 15 + 16) != Z_OK) {
    return std::nullopt;
  }

  ZSetInput(stream, input);

  std::string output;
  std::array<char, 8192> buffer{};
  int ret = Z_OK;
  while (ret == Z_OK) {
    ZSetOutput(stream, buffer.data(), buffer.size());
    ret = ZInflate(stream, Z_NO_FLUSH);
    if (ret != Z_OK && ret != Z_STREAM_END) {
      ZInflateEnd(stream);
      return std::nullopt;
    }
    const auto produced = buffer.size() - stream.avail_out;
    if (produced > 0) {
      output.append(buffer.data(), produced);
    }
  }

  ZInflateEnd(stream);
  return output;
}

struct BenchConfig {
  BenchConfig(uint16_t defaultPort, int argc, char* argv[])
      : port(defaultPort), numThreads(static_cast<uint32_t>(GetNumThreads())) {
    const char* envPort = std::getenv("BENCH_PORT");
    if (envPort != nullptr) {
      port = static_cast<uint16_t>(std::atoi(envPort));
    }
    for (int argPos = 1; argPos < argc; ++argPos) {
      std::string_view arg(argv[argPos]);
      if (arg == "--port" && argPos + 1 < argc) {
        port = static_cast<uint16_t>(std::atoi(argv[++argPos]));
      } else if (arg == "--threads" && argPos + 1 < argc) {
        numThreads = static_cast<uint32_t>(std::atoi(argv[++argPos]));
      } else if (arg == "--tls") {
        tlsEnabled = true;
      } else if (arg == "--cert" && argPos + 1 < argc) {
        certFile = argv[++argPos];
      } else if (arg == "--key" && argPos + 1 < argc) {
        keyFile = argv[++argPos];
      } else if (arg == "--h2") {
        h2Enabled = true;
      } else if (arg == "--static" && argPos + 1 < argc) {
        staticDir = argv[++argPos];
      } else if (arg == "--routes" && argPos + 1 < argc) {
        routeCount = std::atoi(argv[++argPos]);
      } else if (arg == "--help" || arg == "-h") {
        std::cout << "Usage: " << argv[0] << " [options]\n"
                  << "Options:\n"
                  << "  --port N      Listen port (default: " << defaultPort << ", env: BENCH_PORT)\n"
                  << "  --threads N   Worker threads (default: nproc/2, env: BENCH_THREADS)\n"
                  << "  --tls         Enable TLS (requires --cert and --key)\n"
                  << "  --h2          Enable HTTP/2 (h2c cleartext or h2 over TLS)\n"
                  << "  --cert FILE   TLS certificate file (PEM)\n"
                  << "  --key FILE    TLS private key file (PEM)\n"
                  << "  --static DIR  Directory for static file serving\n"
                  << "  --routes N    Number of literal routes (default: 1000)\n"
                  << "  --help        Show this help\n";
        std::exit(0);
      }
    }
  }

  uint16_t port;
  bool tlsEnabled{false};
  bool h2Enabled{false};
  uint32_t numThreads;
  int routeCount{1000};  // Number of literal routes for routing stress test
  std::string_view certFile;
  std::string_view keyFile;
  std::string_view staticDir;
};

}  // namespace bench