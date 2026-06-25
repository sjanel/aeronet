#pragma once

// Shared gzip-decode helper for the competitor drivers (curl / drogon / beast).
//
// For the `compress` scenario the server gzips the response; aeronet decodes it natively (its built-in
// content-coding, backed by zlib-ng). To keep the comparison fair, the other clients do NOT use their own
// bundled inflate — they decode the raw gzip body with the very same zlib-ng (native `zng_*` API, exactly
// as aeronet and the scripted *server* benchmarks do). So the only thing that differs across clients is the
// integration (automatic vs. an explicit decode step), not the codec implementation.

#include <cstdint>
#include <string_view>

#ifdef AERONET_BENCH_HAVE_ZLIBNG
#include <zlib-ng.h>
#endif

namespace aeronet::bench {

// Inflate a gzip body with zlib-ng and return the decoded byte count. Passthrough (returns data.size())
// when the body is not gzip-framed (covers a client that already auto-decoded) or when zlib-ng is not
// compiled in. Returns -1 on a corrupt stream.
inline long GunzipDecodedSize(std::string_view data) {
#ifdef AERONET_BENCH_HAVE_ZLIBNG
  if (data.size() >= 2 && static_cast<unsigned char>(data[0]) == 0x1F &&
      static_cast<unsigned char>(data[1]) == 0x8B) {
    zng_stream zs{};
    if (zng_inflateInit2(&zs, 15 + 16) != Z_OK) {  // 15 + 16 => gzip framing
      return -1;
    }
    zs.next_in = reinterpret_cast<const uint8_t*>(data.data());
    zs.avail_in = static_cast<uint32_t>(data.size());
    unsigned char buffer[16384];
    long total = 0;
    int ret = Z_OK;
    do {
      zs.next_out = buffer;
      zs.avail_out = sizeof(buffer);
      ret = zng_inflate(&zs, Z_NO_FLUSH);
      if (ret != Z_OK && ret != Z_STREAM_END) {
        zng_inflateEnd(&zs);
        return -1;
      }
      total += static_cast<long>(sizeof(buffer) - zs.avail_out);
    } while (ret != Z_STREAM_END);
    zng_inflateEnd(&zs);
    return total;
  }
#endif
  return static_cast<long>(data.size());
}

}  // namespace aeronet::bench
