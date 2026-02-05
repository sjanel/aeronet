#include "aeronet/zlib-stream-raii.hpp"

#include <zconf.h>
#include <zlib.h>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <utility>

#include "aeronet/buffer-cache.hpp"
#include "aeronet/log.hpp"

namespace aeronet {

namespace {

[[nodiscard]] constexpr int ComputeWindowBits(ZStreamRAII::Variant variant) {
  switch (variant) {
    case ZStreamRAII::Variant::gzip:
      return MAX_WBITS + 16;
    case ZStreamRAII::Variant::deflate:
      return MAX_WBITS;
    default:
      throw std::invalid_argument("Invalid zlib variant");
  }
}

voidpf ZAlloc(voidpf opaque, unsigned items, unsigned size) {
  return static_cast<internal::BufferCache*>(opaque)->allocate(static_cast<std::size_t>(items) * size);
}

void ZFree(voidpf opaque, voidpf address) noexcept { static_cast<internal::BufferCache*>(opaque)->deallocate(address); }

}  // namespace

void ZStreamRAII::initZcache() {
  stream = {};
  stream.zalloc = ZAlloc;
  stream.zfree = ZFree;
  stream.opaque = &_cache;
}

ZStreamRAII::ZStreamRAII(ZStreamRAII&& rhs) noexcept
    : _cache(std::move(rhs._cache)), _variant(rhs._variant), _mode(rhs._mode), _level(rhs._level) {
  rhs.end();
}

ZStreamRAII& ZStreamRAII::operator=(ZStreamRAII&& rhs) noexcept {
  if (this != &rhs) [[likely]] {
    const auto variant = rhs._variant;
    const auto mode = rhs._mode;
    const auto level = rhs._level;

    _cache = std::move(rhs._cache);

    end();
    rhs.end();

    _variant = variant;
    _mode = mode;
    _level = level;
  }
  return *this;
}

void ZStreamRAII::initCompress(Variant variant, int8_t level) {
  if (_variant == variant) {
    assert(_mode == Mode::compress);
    // Reuse existing deflate state by resetting it
    const auto ret = deflateReset(&stream);
    if (ret != Z_OK) [[unlikely]] {
      throw std::runtime_error("Error from deflateReset");
    }

    if (level != _level) {
      // Update compression level if different
      const auto retLevel = deflateParams(&stream, level, Z_DEFAULT_STRATEGY);
      if (retLevel != Z_OK) [[unlikely]] {
        throw std::runtime_error("Error from deflateParams");
      }
      _level = level;
    }
  } else {
    end();

    initZcache();

    const auto ret = deflateInit2(&stream, level, Z_DEFLATED, ComputeWindowBits(variant), 8, Z_DEFAULT_STRATEGY);
    if (ret != Z_OK) [[unlikely]] {
      throw std::runtime_error("Error from deflateInit2");
    }

    _variant = variant;
    _mode = Mode::compress;
    _level = level;
  }
}

void ZStreamRAII::initDecompress(Variant variant) {
  if (_variant == Variant::uninitialized) {
    initZcache();

    const auto ret = inflateInit2(&stream, ComputeWindowBits(variant));
    if (ret != Z_OK) [[unlikely]] {
      throw std::runtime_error("Error from inflateInit2");
    }
    _variant = variant;
    _mode = Mode::decompress;
  } else if (_variant == variant) {
    assert(_mode == Mode::decompress);
    // Reuse existing inflate state by resetting it
    const auto ret = inflateReset(&stream);
    if (ret != Z_OK) [[unlikely]] {
      throw std::runtime_error("Error from inflateReset");
    }
  } else {
    assert(_mode == Mode::decompress);
    const auto ret = inflateReset2(&stream, ComputeWindowBits(variant));
    if (ret != Z_OK) [[unlikely]] {
      throw std::runtime_error("Error from inflateReset2");
    }

    _variant = variant;
  }
}

void ZStreamRAII::end() noexcept {
  auto ret = Z_OK;
  switch (_mode) {
    case Mode::decompress:
      ret = inflateEnd(&stream);
      break;
    case Mode::compress:
      ret = deflateEnd(&stream);
      break;
    default:
      assert(_mode == Mode::uninitialized);
      return;  // nothing to clean up
  }
  if (ret != Z_OK) [[unlikely]] {
    log::error("zlib: end returned {} (ignored)", ret);
  }
  _variant = Variant::uninitialized;
  _mode = Mode::uninitialized;
  _level = 0;
}

}  // namespace aeronet