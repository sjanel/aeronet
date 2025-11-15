#pragma once

#include <sys/socket.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <random>
#include <string>
#include <thread>

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

// Simplified blocking implementation: issue request, block until headers, then read body per Content-Length.
// This intentionally forgoes adaptive/non-blocking complexity to reduce flakiness.
inline std::optional<std::size_t> requestBodySize(std::string_view method, std::string_view path, int fd,
                                                  std::size_t requestedSize, bool keepAlive) {
  std::string req(method);
  req.push_back(' ');
  req.append(path);
  req.append("?size=");
  req.append(std::to_string(requestedSize));
  req.append(" HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: ");
  req.append(keepAlive ? aeronet::http::keepalive : aeronet::http::close);
  req.append(aeronet::http::DoubleCRLF);

  aeronet::test::sendAll(fd, req);

  // Extend deadline for benchmarks that request very large bodies over fresh TCP connections.
  // Keep tests / CI responsive while avoiding spurious timeouts for multi-megabyte responses.
  constexpr auto kGlobalDeadline = std::chrono::seconds(30);
  const auto deadline = std::chrono::steady_clock::now() + kGlobalDeadline;
  aeronet::RawChars buffer;
  std::size_t contentLength = 0;
  bool haveCL = false;
  // 1. Read headers
  while (std::chrono::steady_clock::now() < deadline) {
    constexpr std::size_t kChunkSize = static_cast<std::size_t>(64) * 1024ULL;
    std::size_t oldSize = buffer.size();

    buffer.ensureAvailableCapacityExponential(kChunkSize);
    for (;;) {
      ssize_t recvBytes = ::recv(fd, buffer.data() + oldSize, kChunkSize, 0);  // blocking
      if (recvBytes > 0) {
        buffer.addSize(static_cast<std::size_t>(recvBytes));
        break;
      }
      if (recvBytes == -1 && errno == EINTR) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;  // retry
      }
      aeronet::log::debug("connection closed before headers: {}", std::strerror(errno));
      return std::nullopt;
    }

    auto crlrRg = std::ranges::search(buffer, aeronet::http::DoubleCRLF);
    if (crlrRg.empty()) {
      continue;
    }
    std::string_view headers(buffer.data(), crlrRg.data() + aeronet::http::DoubleCRLF.size());
    auto clPos = headers.find("Content-Length:");
    if (clPos == std::string_view::npos) {
      clPos = headers.find("content-length:");
    }
    bool isChunked = false;
    if (clPos == std::string_view::npos) {
      // Check for Transfer-Encoding: chunked (case-insensitive search)
      auto tePos = headers.find("Transfer-Encoding:");
      if (tePos == std::string_view::npos) {
        tePos = headers.find("transfer-encoding:");
      }
      if (tePos != std::string_view::npos) {
        // crude check for the token 'chunked' after the header name
        auto teEnd = headers.find('\r', tePos);
        if (teEnd == std::string_view::npos) {
          teEnd = headers.size();
        }
        std::string_view teVal = headers.substr(tePos, teEnd - tePos);
        if (teVal.find("chunked") != std::string_view::npos || teVal.find("CHUNKED") != std::string_view::npos) {
          isChunked = true;
        }
      }
      if (!isChunked) {
        aeronet::log::error("Content-Length missing");
        return std::nullopt;
      }
    }
    clPos += sizeof("Content-Length:") - 1;
    while (clPos < headers.size() && (headers[clPos] == ' ' || headers[clPos] == '\t')) {
      ++clPos;
    }
    while (clPos < headers.size() && headers[clPos] >= '0' && headers[clPos] <= '9') {
      contentLength = (contentLength * 10) + static_cast<std::size_t>(headers[clPos] - '0');
      haveCL = true;
      ++clPos;
      if (contentLength > (1U << 30)) {
        break;  // cap 1GB
      }
    }
    if (!haveCL) {
      aeronet::log::error("failed to parse Content-Length");
      return std::nullopt;
    }
    std::size_t bodyOffset = static_cast<std::size_t>(crlrRg.data() - buffer.data()) + aeronet::http::DoubleCRLF.size();
    std::size_t haveBody = buffer.size() - bodyOffset;
    if (!isChunked) {
      if (contentLength == 0) {
        return 0U;
      }
      if (haveBody >= contentLength) {
        return contentLength;
      }
      // 2. Read remaining body for Content-Length
      std::size_t remaining = contentLength - haveBody;
      while (remaining > 0 && std::chrono::steady_clock::now() < deadline) {
        std::size_t chunkSize = std::min<std::size_t>(static_cast<std::size_t>(64) * 1024ULL, remaining);
        std::size_t oldBodySize = buffer.size();

        buffer.ensureAvailableCapacityExponential(chunkSize);

        for (;;) {
          ssize_t recvBytes = ::recv(fd, buffer.data() + oldBodySize, chunkSize, 0);
          if (recvBytes > 0) {
            buffer.addSize(static_cast<std::size_t>(recvBytes));
            break;
          }
          if (recvBytes == -1 && errno == EINTR) {
            continue;  // retry
          }
          aeronet::log::debug("body truncated: wanted {}, have {}", contentLength, contentLength - remaining);
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
          break;
        }
        remaining -= buffer.size() - oldBodySize;
      }
      if (remaining == 0) {
        return contentLength;
      }
    } else {
      // Chunked transfer decoding: read chunks until a 0-sized chunk is seen.
      std::size_t totalBody = 0;
      std::size_t curPos = bodyOffset;
      auto readMore = [&](std::size_t want) {
        while (buffer.size() < want && std::chrono::steady_clock::now() < deadline) {
          std::size_t oldSize = buffer.size();
          static constexpr std::size_t kChunkSize = static_cast<std::size_t>(64) * 1024ULL;

          buffer.ensureAvailableCapacityExponential(kChunkSize);
          for (;;) {
            ssize_t r = ::recv(fd, buffer.data() + oldSize, kChunkSize, 0);
            if (r > 0) {
              buffer.addSize(static_cast<std::size_t>(r));
              break;
            }
            if (r == -1 && errno == EINTR) {
              continue;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
          }
        }
      };
      for (;;) {
        // Ensure we have at least a line for the chunk size
        readMore(curPos + 32);
        // find CRLF
        auto lineEnd = std::string_view(buffer.data() + curPos, buffer.size() - curPos);
        auto posCr = lineEnd.find(aeronet::http::CRLF);
        while (posCr == std::string_view::npos && std::chrono::steady_clock::now() < deadline) {
          readMore(buffer.size() + 32);
          lineEnd = std::string_view(buffer.data() + curPos, buffer.size() - curPos);
          posCr = lineEnd.find(aeronet::http::CRLF);
        }
        if (posCr == std::string_view::npos) {
          aeronet::log::error("timeout reading chunk size");
          return std::nullopt;
        }
        std::string_view sizeLine(buffer.data() + curPos, posCr);
        // parse hex chunk size using shared helper
        std::size_t chunkSz = 0;
        for (char ch : sizeLine) {
          int digit = aeronet::from_hex_digit(ch);
          if (digit < 0) {
            break;  // ignore chunk extensions or invalid digits
          }
          chunkSz = (chunkSz << 4) + static_cast<std::size_t>(digit);
        }
        curPos += posCr + 2;  // skip CRLF
        // Ensure we have the whole chunk (chunkSz bytes + CRLF)
        readMore(curPos + chunkSz + 2);
        if (buffer.size() < curPos + chunkSz + 2) {
          aeronet::log::error("timeout reading chunk payload");
          return std::nullopt;
        }
        totalBody += chunkSz;
        curPos += chunkSz;
        // expect CRLF after chunk
        if (curPos + aeronet::http::CRLF.size() > buffer.size() ||
            !std::equal(aeronet::http::CRLF.begin(), aeronet::http::CRLF.end(), buffer.begin() + curPos)) {
          aeronet::log::error("malformed chunked encoding");
          return std::nullopt;
        }
        curPos += aeronet::http::CRLF.size();
        if (chunkSz == 0) {
          return totalBody;
        }
      }
    }
    aeronet::log::error("timeout reading body");
    return std::nullopt;
  }
  aeronet::log::error("timeout waiting for headers ({}s cap)", kGlobalDeadline.count());
  return std::nullopt;
}

}  // namespace benchutil
