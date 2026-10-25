#include "aeronet/dynamic-concatenated-strings.hpp"

#include <stdexcept>
#include <string_view>

#include "aeronet/http-constants.hpp"
#include "aeronet/log.hpp"

namespace aeronet {

void ThrowDynamicConcatenatedStringsPartContainsSeparator(std::string_view part, std::string_view sep) {
  log::critical("DynamicConcatenatedStrings::append: part of size {} contains the separator at offset {}", part.size(),
                part.find(sep));
  throw std::invalid_argument("DynamicConcatenatedStrings::append: part contains separator");
}

void ThrowDynamicConcatenatedStringsNameOrValueContainsSeparator(std::string_view name, std::string_view value) {
  log::critical("DynamicConcatenatedStrings::appendAsHttp1Header: {} contains CRLF (name size {}, value size {})",
                name.contains(http::CRLF) ? "name" : "value", name.size(), value.size());
  throw std::invalid_argument("DynamicConcatenatedStrings::appendAsHttp1Header: name or value contains separator");
}

}  // namespace aeronet
