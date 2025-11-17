#pragma once

#include <string_view>

#include "aeronet/concatenated-headers.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/raw-chars.hpp"

namespace aeronet {

RawChars BuildSimpleError(http::StatusCode status, const ConcatenatedHeaders& globalHeaders, std::string_view reason);

}  // namespace aeronet
