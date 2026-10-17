#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include "aeronet/http-client.hpp"
#include "aeronet/raw-chars.hpp"

namespace aeronet {

// Access to the internals of HttpClient for tests (friend of HttpClient).
class HttpClientTest {
 public:
  // Number of origins with an entry in the idle pool (possibly empty).
  static std::size_t IdleOrigins(const HttpClient& client) { return client._idle.size(); }

  // Total number of pooled idle connections, all origins together.
  static std::size_t IdleConnections(const HttpClient& client) {
    std::size_t nb = 0;
    for (const auto& [origin, bucket] : client._idle) {
      nb += bucket.size();
    }
    return nb;
  }

  static std::size_t CacheEntries(const HttpClient& client) { return client._cache.size(); }

  // Whether `ptr` points into one of the scratch buffers holding received or decoded response bytes.
  static bool InScratch(const HttpClient& client, const char* ptr) {
    const auto address = reinterpret_cast<std::uintptr_t>(ptr);
    return std::ranges::any_of(std::array{&client._responseBuffer, &client._reqBodyScratch,
                                          &client._codec.decompressOut, &client._codec.decompressTmp},
                               [address](const RawChars* buf) {
                                 const auto begin = reinterpret_cast<std::uintptr_t>(buf->data());
                                 return address >= begin && address < begin + buf->size();
                               });
  }

  // Summed capacity of the reusable request / response / decompression scratch buffers.
  static std::size_t ScratchCapacity(const HttpClient& client) {
    std::size_t capacity = client._cacheKeyScratch.capacity() + client._reqBodyScratch.capacity() +
                           client._responseBuffer.capacity() + client._codec.decompressOut.capacity() +
                           client._codec.decompressTmp.capacity();
#ifdef AERONET_ENABLE_HTTP2
    capacity += client._outputFragmentsScratch.capacity();
#endif
    return capacity;
  }
};

}  // namespace aeronet
