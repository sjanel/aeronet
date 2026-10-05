#include "aeronet/zlib-stream-raii.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <utility>

#include "aeronet/buffer-cache.hpp"
#include "aeronet/zlib-gateway.hpp"

#ifdef AERONET_ENABLE_ZLIBNG
#include <zconf-ng.h>
#else
#include <zconf.h>
#endif

namespace aeronet {

namespace {

[[nodiscard]] constexpr int ComputeWindowBits(ZStreamRAII::Variant variant, uint8_t windowBits) {
  static_assert(ZStreamRAII::kMaxWindowBits == MAX_WBITS);
  switch (variant) {
    case ZStreamRAII::Variant::gzip:
      return windowBits + 16;
    case ZStreamRAII::Variant::deflate:
      return windowBits;
    case ZStreamRAII::Variant::raw:
      return -static_cast<int>(windowBits);
    default:
      throw std::invalid_argument("Invalid zlib variant");
  }
}

void* ZAlloc(void* opaque, unsigned items, unsigned size) {
  return static_cast<internal::BufferCache*>(opaque)->allocate(static_cast<std::size_t>(items) * size);
}

void ZFree(void* opaque, void* address) noexcept { static_cast<internal::BufferCache*>(opaque)->deallocate(address); }

}  // namespace

void ZStreamRAII::initZcache() {
  stream = {};
  stream.zalloc = ZAlloc;
  stream.zfree = ZFree;
  stream.opaque = &_cache;
}

ZStreamRAII::ZStreamRAII(ZStreamRAII&& rhs) noexcept
    : _cache(std::move(rhs._cache)),
      _variant(rhs._variant),
      _mode(rhs._mode),
      _level(rhs._level),
      _windowBits(rhs._windowBits) {
  rhs.end();
}

ZStreamRAII& ZStreamRAII::operator=(ZStreamRAII&& rhs) noexcept {
  if (this != &rhs) [[likely]] {
    const auto variant = rhs._variant;
    const auto mode = rhs._mode;
    const auto level = rhs._level;
    const auto windowBits = rhs._windowBits;

    _cache = std::move(rhs._cache);

    end();
    rhs.end();

    _variant = variant;
    _mode = mode;
    _level = level;
    _windowBits = windowBits;
  }
  return *this;
}

namespace {

constexpr auto CheckError = [](auto ret, const char* pMsg) {
  if (ret != Z_OK) [[unlikely]] {
    throw std::runtime_error(pMsg);
  }
};

}  // namespace

void ZStreamRAII::initCompress(Variant variant, int8_t level, uint8_t windowBits) {
  if (_variant == variant && _windowBits == windowBits) {
    assert(_mode == Mode::compress);
    // Reuse existing deflate state by resetting it
    CheckError(ZDeflateReset(stream), "Error from ZDeflateReset");

    if (level != _level) {
      // Update compression level if different
      CheckError(ZDeflateParams(stream, level, Z_DEFAULT_STRATEGY), "Error from ZDeflateParams");
      _level = level;
    }
  } else {
    end();

    initZcache();

    CheckError(ZDeflateInit2(stream, level, Z_DEFLATED, ComputeWindowBits(variant, windowBits), 8, Z_DEFAULT_STRATEGY),
               "Error from ZDeflateInit2");

    _variant = variant;
    _mode = Mode::compress;
    _level = level;
    _windowBits = windowBits;
  }
}

void ZStreamRAII::initDecompress(Variant variant, uint8_t windowBits) {
  if (_variant == Variant::uninitialized) {
    initZcache();
    CheckError(ZInflateInit2(stream, ComputeWindowBits(variant, windowBits)), "Error from ZInflateInit2");
    _variant = variant;
    _mode = Mode::decompress;
    _windowBits = windowBits;
  } else if (_variant == variant && _windowBits == windowBits) {
    assert(_mode == Mode::decompress);
    // Reuse existing inflate state by resetting it
    CheckError(ZInflateReset(stream), "Error from ZInflateReset");
  } else {
    assert(_mode == Mode::decompress);
    CheckError(ZInflateReset2(stream, ComputeWindowBits(variant, windowBits)), "Error from ZInflateReset2");
    _variant = variant;
    _windowBits = windowBits;
  }
}

void ZStreamRAII::end() noexcept {
  [[maybe_unused]] auto ret = Z_OK;
  switch (_mode) {
    case Mode::decompress:
      ret = ZInflateEnd(stream);
      break;
    case Mode::compress:
      ret = ZDeflateEnd(stream);
      break;
    default:
      assert(_mode == Mode::uninitialized);
      return;  // nothing to clean up
  }
  // The stream is freed in any case. Z_DATA_ERROR only reports a compression stream that was not finished: expected for
  // permessage-deflate streams (sync flushes only) and for compressed responses interrupted by a peer disconnection.
  // Other errors denote an inconsistent stream state, a programming error.
  assert(ret == Z_OK || ret == Z_DATA_ERROR);
  _variant = Variant::uninitialized;
  _mode = Mode::uninitialized;
  _level = 0;
  _windowBits = kMaxWindowBits;
}

}  // namespace aeronet