#include "aeronet/errno-throw.hpp"

#include <cstddef>
#include <string_view>
#include <system_error>

#include "aeronet/format-lite.hpp"
#include "aeronet/raw-chars.hpp"

namespace aeronet::detail {

void ThrowFormattedSystemError(int err, std::string_view fmt, const FormatArg* pArgs, std::size_t nbArgs) {
#ifdef AERONET_WINDOWS
  const std::error_code ec(err, std::system_category());
#else
  const std::error_code ec(err, std::generic_category());
#endif
  RawChars message(fmt.size() + (16U * nbArgs) + 1U);  // + 1: the final null char
  VFormatTo(message, fmt, pArgs, nbArgs);
  message.push_back('\0');
  throw std::system_error(ec, message.data());
}

}  // namespace aeronet::detail
