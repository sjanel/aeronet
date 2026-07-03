#pragma once

#include <cstddef>
#include <string_view>

#include "aeronet/encoding.hpp"
#include "aeronet/http-constants.hpp"

namespace aeronet::internal {

// Comma-separated list of the content codings this build can decode, in aeronet's preference order.
// Advertised as the default Accept-Encoding when response decompression is enabled and the user did not
// set an explicit value. Built once at compile time from the codecs compiled in. Shared by the HTTP/1.1
// and HTTP/2 client request builders.
namespace details {
struct AcceptEncodingCoding {
  std::string_view name;
  bool enabled;
};

// Preference order; gzip and deflate both ride on zlib.
inline constexpr AcceptEncodingCoding kAcceptEncodingCodings[]{
    {http::zstd, IsEncodingEnabled(Encoding::zstd)},
    {http::br, IsEncodingEnabled(Encoding::br)},
    {http::gzip, IsEncodingEnabled(Encoding::gzip)},
    {http::deflate, IsEncodingEnabled(Encoding::deflate)},
};

// Exact byte length of the comma-separated list of the enabled codings (separators included).
constexpr std::size_t ComputeAcceptEncodingSize() {
  std::size_t size = 0;
  for (const auto& coding : kAcceptEncodingCodings) {
    if (coding.enabled) {
      if (size != 0) {
        size += 2;  // ", " separator
      }
      size += coding.name.size();
    }
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
  for (const auto& coding : kAcceptEncodingCodings) {
    if (coding.enabled) {
      if (pos != 0) {
        out.storage[pos++] = ',';
        out.storage[pos++] = ' ';
      }
      for (char ch : coding.name) {
        out.storage[pos++] = ch;
      }
    }
  }
  return out;
}

inline constexpr AcceptEncodingStorage kAcceptEncodingStorage = MakeAcceptEncoding();
}  // namespace details

inline constexpr std::string_view kSupportedAcceptEncoding{details::kAcceptEncodingStorage.storage,
                                                           details::ComputeAcceptEncodingSize()};

}  // namespace aeronet::internal
