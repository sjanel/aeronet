#pragma once

#include <sys/socket.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <random>
#include <string>

#include "aeronet/char-hexadecimal-converter.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/log.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/test_util.hpp"

// (Pregen pools are declared inside the benchutil namespace below)

namespace benchutil {

inline constexpr std::string kBodyPath = "/body";
inline constexpr std::string kHeaderPath = "/headers";

// Pre-generated string pools to avoid per-request RNG or allocation noise during benchmarks.
struct PregenPool {
  std::vector<std::string> items;
  std::atomic<uint32_t> idx{0};
  // Each pool owns its RNG so we can deterministically re-seed it when
  // pre-filling. This ensures the same sequence of strings across runs.
  std::mt19937_64 rng{42};
  std::string pregen;
  std::size_t minSz;
  std::size_t maxSz;
  std::uniform_int_distribution<std::size_t> dist;

  PregenPool() = default;

  // Construct and fill using the pool's own RNG (deterministic seed).
  PregenPool(size_t nbStr, size_t minStrSz, size_t maxStrSz, uint64_t seed = 0)
      : minSz(minStrSz), maxSz(maxStrSz), dist(minStrSz, maxStrSz) {
    reset(nbStr, minStrSz, maxStrSz, seed);
  }

  std::string randomStr(std::size_t n) {
    std::string out(n, 'X');  // make it start with X to avoid conflicts with HTTP known headers
    static constexpr char charset[] =
        "0123456789"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz";
    std::uniform_int_distribution<std::size_t> dist(0, std::size(charset) - 2);
    for (std::size_t charPos = 1; charPos < out.size(); ++charPos) {
      out[charPos] = charset[dist(rng)];
    }

    return out;
  }

  // Reset the pool deterministically. Clears existing items but retains
  // capacity to avoid repeated allocations across runs.
  void reset(size_t nbStr, size_t minStrSz, size_t maxStrSz, uint64_t seed = 0) {
    idx.store(0, std::memory_order_relaxed);
    rng.seed(seed);
    items.clear();
    items.reserve(nbStr);

    minSz = minStrSz;
    maxSz = maxStrSz;

    dist = std::uniform_int_distribution<std::size_t>(minStrSz, maxStrSz);

    pregen = randomStr(dist(rng));

    for (size_t strPos = 0; strPos < nbStr; ++strPos) {
      size_t sz = dist(rng);
      items.push_back(randomStr(sz));
    }
  }

  [[nodiscard]] std::size_t nextSize() const {
    if (idx.load(std::memory_order_relaxed) == items.size()) {
      return pregen.size();
    }
    return items[idx.load(std::memory_order_relaxed)].size();
  }

  std::string next() {
    if (idx.load(std::memory_order_relaxed) == items.size()) {
      auto str = std::move(pregen);
      size_t sz = dist(rng);
      pregen = randomStr(sz);
      return str;
    }
    return std::move(items[idx.fetch_add(1, std::memory_order_relaxed)]);
  }
};

// Iterate over a header block produced by the pools. The block format is a sequence of
// "key\0value\0" entries. The callback is invoked with (key_sv, val_sv) for each
// parsed pair. The helper is templated to accept any callable without allocations.
template <typename Fn>
inline void forEachHeader(std::string_view hdrBlock, Fn &&cb) {
  const char *cur = hdrBlock.data();
  const char *end = cur + hdrBlock.size();
  while (cur < end) {
    const char *keyPtr = cur;
    const char *valPtr = static_cast<const char *>(memchr(cur, '\0', static_cast<size_t>(end - cur)));
    if (valPtr == nullptr) {
      break;
    }
    ++valPtr;
    const char *nextPtr = static_cast<const char *>(memchr(valPtr, '\0', static_cast<size_t>(end - valPtr)));
    if (nextPtr == nullptr) {
      break;
    }
    const std::size_t keyLen = static_cast<std::size_t>(valPtr - keyPtr - 1);
    const std::size_t valLen = static_cast<std::size_t>(nextPtr - valPtr);
    std::string_view key(keyPtr, keyLen);
    std::string_view val(valPtr, valLen);
    cb(key, val);
    cur = nextPtr + 1;
  }
}

// Blocking recv helper - returns bytes read, 0 on close, -1 on timeout.
inline ssize_t blockingRecv(int fd, char *buf, std::size_t len, std::chrono::steady_clock::time_point deadline) {
  while (std::chrono::steady_clock::now() < deadline) {
    ssize_t nb = ::recv(fd, buf, len, 0);
    if (nb > 0) {
      return nb;
    }
    if (nb == 0) {
      return 0;  // connection closed
    }
    if (errno == EINTR) {
      continue;
    }
    return 0;  // error treated as close
  }
  return -1;  // timeout
}

// Parse Content-Length value starting at pos in headers. Returns (value, success).
inline std::pair<std::size_t, bool> parseContentLength(std::string_view headers, std::size_t pos) {
  // Skip "Content-Length:" prefix
  pos += 15;
  // Skip whitespace
  while (pos < headers.size() && (headers[pos] == ' ' || headers[pos] == '\t')) {
    ++pos;
  }
  std::size_t val = 0;
  bool found = false;
  while (pos < headers.size() && headers[pos] >= '0' && headers[pos] <= '9') {
    val = (val * 10) + static_cast<std::size_t>(headers[pos] - '0');
    found = true;
    ++pos;
    if (val > (1ULL << 30)) {
      break;  // cap 1GB
    }
  }
  return {val, found};
}

// Check if headers contain chunked transfer encoding.
inline bool isChunkedEncoding(std::string_view headers) {
  auto pos = headers.find("Transfer-Encoding:");
  if (pos == std::string_view::npos) {
    pos = headers.find("transfer-encoding:");
  }
  if (pos == std::string_view::npos) {
    return false;
  }
  auto lineEnd = headers.find('\r', pos);
  std::string_view val = headers.substr(pos, lineEnd == std::string_view::npos ? headers.size() - pos : lineEnd - pos);
  return val.contains("chunked");
}

// Issue HTTP request and return response body size. Optimized for benchmark hot path.
inline std::optional<std::size_t> requestBodySize(std::string_view method, std::string_view path, int fd,
                                                  std::size_t requestedSize, bool keepAlive) {
  // Build request with minimal allocations
  std::string req;
  req.reserve(method.size() + path.size() + 80);
  req.append(method);
  req += ' ';
  req.append(path);
  req += "?size=";
  req += std::to_string(requestedSize);
  req += " HTTP/1.1\r\nHost: h\r\nConnection: ";
  req += keepAlive ? aeronet::http::keepalive : aeronet::http::close;
  req += aeronet::http::DoubleCRLF;

  aeronet::test::sendAll(fd, req);

  static constexpr auto kTimeout = std::chrono::seconds(30);
  const auto deadline = std::chrono::steady_clock::now() + kTimeout;
  static constexpr std::size_t kBufSize = 64ULL * 1024ULL;

  aeronet::RawChars buf(kBufSize);

  // --- Phase 1: Read until we have complete headers ---
  const char *headerEnd = nullptr;
  while (headerEnd == nullptr) {
    ssize_t nb = blockingRecv(fd, buf.end(), buf.availableCapacity(), deadline);
    if (nb <= 0) {
      return std::nullopt;
    }
    buf.addSize(static_cast<std::size_t>(nb));

    // Search for header terminator
    std::string_view data(buf.data(), buf.size());
    auto pos = data.find(aeronet::http::DoubleCRLF);
    if (pos != std::string_view::npos) {
      headerEnd = buf.data() + pos + aeronet::http::DoubleCRLF.size();
    } else {
      buf.ensureAvailableCapacityExponential(kBufSize);
    }
  }

  std::string_view headers(buf.data(), static_cast<std::size_t>(headerEnd - buf.data()));
  std::size_t bodyOffset = static_cast<std::size_t>(headerEnd - buf.data());

  // --- Phase 2: Determine transfer mode and content length ---
  auto clPos = headers.find("Content-Length:");
  if (clPos == std::string_view::npos) {
    clPos = headers.find("content-length:");
  }

  bool chunked = (clPos == std::string_view::npos) && isChunkedEncoding(headers);
  if (clPos == std::string_view::npos && !chunked) {
    aeronet::log::error("No Content-Length or chunked encoding");
    return std::nullopt;
  }

  // --- Phase 3a: Content-Length mode ---
  if (!chunked) {
    auto [contentLength, ok] = parseContentLength(headers, clPos);
    if (!ok) {
      aeronet::log::error("Failed to parse Content-Length");
      return std::nullopt;
    }
    if (contentLength == 0) {
      return 0;
    }

    std::size_t haveBody = buf.size() - bodyOffset;
    while (haveBody < contentLength) {
      buf.ensureAvailableCapacityExponential(kBufSize);
      ssize_t nb = blockingRecv(fd, buf.end(), buf.availableCapacity(), deadline);
      if (nb <= 0) {
        return std::nullopt;
      }
      buf.addSize(static_cast<std::size_t>(nb));
      haveBody = buf.size() - bodyOffset;
    }
    return contentLength;
  }

  // --- Phase 3b: Chunked mode ---
  std::size_t totalBody = 0;
  std::size_t pos = bodyOffset;

  auto ensureBytes = [&](std::size_t need) -> bool {
    while (buf.size() < need) {
      buf.ensureAvailableCapacityExponential(kBufSize);
      ssize_t nb = blockingRecv(fd, buf.end(), buf.availableCapacity(), deadline);
      if (nb <= 0) {
        return false;
      }
      buf.addSize(static_cast<std::size_t>(nb));
    }
    return true;
  };

  for (;;) {
    // Find chunk size line
    if (!ensureBytes(pos + 8)) {
      return std::nullopt;
    }

    const char *lineStart = buf.data() + pos;
    const char *crlfPos = nullptr;
    while (crlfPos == nullptr) {
      std::string_view remaining(buf.data() + pos, buf.size() - pos);
      auto idx = remaining.find(aeronet::http::CRLF);
      if (idx != std::string_view::npos) {
        crlfPos = buf.data() + pos + idx;
      } else if (!ensureBytes(buf.size() + 32)) {
        return std::nullopt;
      }
    }

    // Parse hex chunk size
    std::size_t chunkSz = 0;
    for (const char *pc = lineStart; pc < crlfPos; ++pc) {
      int digit = aeronet::from_hex_digit(*pc);
      if (digit < 0) {
        break;
      }
      chunkSz = (chunkSz << 4) | static_cast<std::size_t>(digit);
    }

    pos = static_cast<std::size_t>(crlfPos - buf.data()) + 2;  // past CRLF

    if (chunkSz == 0) {
      return totalBody;  // final chunk
    }

    // Ensure chunk data + trailing CRLF
    if (!ensureBytes(pos + chunkSz + 2)) {
      return std::nullopt;
    }

    totalBody += chunkSz;
    pos += chunkSz + 2;  // skip data + CRLF
  }
}

}  // namespace benchutil
