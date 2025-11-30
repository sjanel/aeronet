#include "aeronet/zlib-stream-raii.hpp"

#include <zconf.h>
#include <zlib.h>

#include <cstdint>
#include <cstring>
#include <format>
#include <stdexcept>

#include "aeronet/log.hpp"

namespace aeronet {

namespace {
constexpr int ComputeWindowBits(ZStreamRAII::Variant variant) {
  switch (variant) {
    case ZStreamRAII::Variant::gzip:
      return MAX_WBITS + 16;
    case ZStreamRAII::Variant::deflate:
      return MAX_WBITS;
    default:
      throw std::invalid_argument("Invalid zlib variant");
  }
}
}  // namespace

ZStreamRAII::ZStreamRAII(Variant variant) : _allocatedType(AllocatedType::inflate) {
  const auto ret = inflateInit2(&stream, ComputeWindowBits(variant));
  if (ret != Z_OK) {
    throw std::runtime_error(std::format("Error from inflateInit2 - error {}", ret));
  }
}

ZStreamRAII::ZStreamRAII(Variant variant, int8_t level) : _allocatedType(AllocatedType::deflate) {
  const auto ret = deflateInit2(&stream, level, Z_DEFLATED, ComputeWindowBits(variant), 8, Z_DEFAULT_STRATEGY);
  if (ret != Z_OK) {
    throw std::runtime_error(std::format("Error from deflateInit2 - error {}", ret));
  }
}

ZStreamRAII::~ZStreamRAII() {
  switch (_allocatedType) {
    case AllocatedType::inflate: {
      const auto ret = inflateEnd(&stream);
      if (ret != Z_OK) {
        log::error("zlib: inflateEnd returned {} (ignored)", ret);
      }
      break;
    }
    case AllocatedType::deflate: {
      const auto ret = deflateEnd(&stream);
      if (ret != Z_OK) {
        log::error("zlib: deflateEnd returned {} (ignored)", ret);
      }
      break;
    }
  }
}
}  // namespace aeronet