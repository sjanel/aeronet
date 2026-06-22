#include "aeronet/stringconv.hpp"

#include <cstddef>
#include <string_view>

#include "aeronet/log.hpp"

namespace aeronet {

void LogStringToIntegralFailure(std::string_view src) { log::critical("Unable to decode '{}' into integral", src); }

void LogStringToIntegralPartialDecode(std::ptrdiff_t decodedCount, std::string_view src, std::string_view value) {
  log::error("Only '{}' chars from '{}' decoded into integral '{}'", decodedCount, src, value);
}

}  // namespace aeronet
