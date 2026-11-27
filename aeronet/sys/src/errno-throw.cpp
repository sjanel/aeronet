#include "aeronet/errno-throw.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <system_error>

#include "aeronet/format-lite.hpp"

namespace aeronet::detail {

void ThrowFormattedSystemError(int err, std::string_view fmt, const FormatArg* pArgs, std::size_t nbArgs) {
#ifdef AERONET_WINDOWS
  const std::error_code ec(err, std::system_category());
#else
  const std::error_code ec(err, std::generic_category());
#endif
  std::string message;
  VFormatTo(message, fmt, pArgs, nbArgs);
  throw std::system_error(ec, message);
}

}  // namespace aeronet::detail
