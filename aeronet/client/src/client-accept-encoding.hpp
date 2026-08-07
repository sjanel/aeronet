#pragma once

#include <array>
#include <cstddef>
#include <string_view>

#include "aeronet/encoding.hpp"

namespace aeronet::internal {

// Comma-separated list of the content codings this build can decode, in aeronet's preference order.
// Advertised as the default Accept-Encoding when response decompression is enabled and the user did not
// set an explicit value. Built once at compile time from the codecs compiled in. Shared by the HTTP/1.1
// and HTTP/2 client request builders.
namespace details {

inline constexpr std::array<Encoding,
#ifdef AERONET_ENABLE_ZLIB
                            2
#else
                            0
#endif
#ifdef AERONET_ENABLE_BROTLI
                                + 1
#endif
#ifdef AERONET_ENABLE_ZSTD
                                + 1
#endif
                            >
    kSupportedEncodings{
#ifdef AERONET_ENABLE_ZLIB
        Encoding::deflate,
        Encoding::gzip,
#endif
#ifdef AERONET_ENABLE_BROTLI
        Encoding::br,
#endif
#ifdef AERONET_ENABLE_ZSTD
        Encoding::zstd,
#endif
    };

// Exact byte length of the comma-separated list of the enabled codings (separators included).
constexpr std::size_t ComputeAcceptEncodingSize() {
  std::size_t size = 0;
  for (const Encoding encoding : kSupportedEncodings) {
    if (size != 0) {
      size += 2;  // ", " separator
    }
    size += GetEncodingStr(encoding).size();
  }
  return size;
}

// Static storage holding exactly the list bytes; the +1 keeps the array non-empty and null-terminated.
struct AcceptEncodingStorage {
  char storage[ComputeAcceptEncodingSize() + 1]{};
};

constexpr AcceptEncodingStorage MakeAcceptEncoding() {
  AcceptEncodingStorage out;
  std::size_t pos = 0;
  for (const Encoding encoding : kSupportedEncodings) {
    if (pos != 0) {
      out.storage[pos++] = ',';
      out.storage[pos++] = ' ';
    }
    for (char ch : GetEncodingStr(encoding)) {
      out.storage[pos++] = ch;
    }
  }
  return out;
}

inline constexpr AcceptEncodingStorage kAcceptEncodingStorage = MakeAcceptEncoding();
}  // namespace details

inline constexpr std::string_view kSupportedAcceptEncoding{details::kAcceptEncodingStorage.storage,
                                                           details::ComputeAcceptEncodingSize()};

}  // namespace aeronet::internal
