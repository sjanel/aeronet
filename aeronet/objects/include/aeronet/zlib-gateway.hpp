#pragma once

#include <cstddef>
#include <string_view>

#ifdef AERONET_ENABLE_ZLIBNG
#include <zlib-ng.h>

#include <cstdint>
#else
#include <zlib.h>
#endif

namespace aeronet {

#ifdef AERONET_ENABLE_ZLIBNG
using zstream = zng_stream;
#else
using zstream = z_stream;
#endif

inline void ZSetInput(zstream& stream, std::string_view data) {
#ifdef AERONET_ENABLE_ZLIBNG
  stream.next_in = reinterpret_cast<const uint8_t*>(data.data());
  stream.avail_in = static_cast<uint32_t>(data.size());
#else
  stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
  stream.avail_in = static_cast<uInt>(data.size());
#endif
}

inline void ZSetOutput(zstream& stream, char* data, std::size_t capacity) {
#ifdef AERONET_ENABLE_ZLIBNG
  stream.next_out = reinterpret_cast<uint8_t*>(data);
  stream.avail_out = static_cast<uint32_t>(capacity);
#else
  stream.next_out = reinterpret_cast<Bytef*>(data);
  stream.avail_out = static_cast<uInt>(capacity);
#endif
}

inline auto ZInflate(zstream& stream, int flush) {
#ifdef AERONET_ENABLE_ZLIBNG
  return zng_inflate(&stream, flush);
#else
  return inflate(&stream, flush);
#endif
}

inline auto ZDeflate(zstream& stream, int flush) {
#ifdef AERONET_ENABLE_ZLIBNG
  return zng_deflate(&stream, flush);
#else
  return deflate(&stream, flush);
#endif
}

inline auto ZDeflateBound(zstream* stream, std::size_t sourceLen) {
#ifdef AERONET_ENABLE_ZLIBNG
  return zng_deflateBound(stream, static_cast<unsigned long>(sourceLen));
#else
  return deflateBound(stream, static_cast<unsigned long>(sourceLen));
#endif
}

inline auto ZDeflateReset(zstream& stream) {
#ifdef AERONET_ENABLE_ZLIBNG
  return zng_deflateReset(&stream);
#else
  return deflateReset(&stream);
#endif
}

inline auto ZDeflateParams(zstream& stream, int level, int strategy) {
#ifdef AERONET_ENABLE_ZLIBNG
  return zng_deflateParams(&stream, level, strategy);
#else
  return deflateParams(&stream, level, strategy);
#endif
}

inline auto ZDeflateInit2(zstream& stream, int level, int method, int windowBits, int memLevel, int strategy) {
#ifdef AERONET_ENABLE_ZLIBNG
  return zng_deflateInit2(&stream, level, method, windowBits, memLevel, strategy);
#else
  return deflateInit2(&stream, level, method, windowBits, memLevel, strategy);
#endif
}

inline auto ZInflateInit2(zstream& stream, int windowBits) {
#ifdef AERONET_ENABLE_ZLIBNG
  return zng_inflateInit2(&stream, windowBits);
#else
  return inflateInit2(&stream, windowBits);
#endif
}

inline auto ZInflateReset(zstream& stream) {
#ifdef AERONET_ENABLE_ZLIBNG
  return zng_inflateReset(&stream);
#else
  return inflateReset(&stream);
#endif
}

inline auto ZInflateReset2(zstream& stream, int windowBits) {
#ifdef AERONET_ENABLE_ZLIBNG
  return zng_inflateReset2(&stream, windowBits);
#else
  return inflateReset2(&stream, windowBits);
#endif
}

inline auto ZInflateEnd(zstream& stream) {
#ifdef AERONET_ENABLE_ZLIBNG
  return zng_inflateEnd(&stream);
#else
  return inflateEnd(&stream);
#endif
}

inline auto ZDeflateEnd(zstream& stream) {
#ifdef AERONET_ENABLE_ZLIBNG
  return zng_deflateEnd(&stream);
#else
  return deflateEnd(&stream);
#endif
}

}  // namespace aeronet
