#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

#ifdef AERONET_ENABLE_ZLIBNG
#include <zlib-ng.h>
#else
#include <zlib.h>
#endif

namespace aeronet {

#ifdef AERONET_ENABLE_ZLIBNG
using zstream = zng_stream;
#else
using zstream = z_stream;
#endif

// zlib counts the input and output buffer sizes in 32 bits: larger buffers are given in slices.
inline constexpr std::size_t kZMaxSize = std::numeric_limits<uint32_t>::max();

// Largest input slice given to one deflate() call: its deflate bound fits in 32 bits (deflateBound() counts in unsigned
// long, of 32 bits on Windows), so an output room clamped to kZMaxSize is always enough for it.
inline constexpr std::size_t kZMaxDeflateInputSize = std::size_t{1} << 30U;

// Clamps a buffer size to what zlib can count.
[[nodiscard]] inline uint32_t ZClampSize(std::size_t size) {
  return size < kZMaxSize ? static_cast<uint32_t>(size) : static_cast<uint32_t>(kZMaxSize);
}

inline void ZSetInput(zstream& stream, const char* pData, uint32_t size) {
#ifdef AERONET_ENABLE_ZLIBNG
  stream.next_in = reinterpret_cast<const uint8_t*>(pData);
  stream.avail_in = size;
#else
  stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(pData));
  stream.avail_in = static_cast<uInt>(size);
#endif
}

inline void ZSetOutput(zstream& stream, char* pData, uint32_t capacity) {
#ifdef AERONET_ENABLE_ZLIBNG
  stream.next_out = reinterpret_cast<uint8_t*>(pData);
  stream.avail_out = capacity;
#else
  stream.next_out = reinterpret_cast<Bytef*>(pData);
  stream.avail_out = static_cast<uInt>(capacity);
#endif
}

// End of the output written by the last deflate() / inflate() call.
[[nodiscard]] inline char* ZOutputEnd(const zstream& stream) { return reinterpret_cast<char*>(stream.next_out); }

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

inline std::size_t ZCompressBound(std::size_t sourceLen) {
#ifdef AERONET_ENABLE_ZLIBNG
  return static_cast<std::size_t>(zng_compressBound(static_cast<unsigned long>(sourceLen)));
#else
  return static_cast<std::size_t>(compressBound(static_cast<unsigned long>(sourceLen)));
#endif
}

}  // namespace aeronet
