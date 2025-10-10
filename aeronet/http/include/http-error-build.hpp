#pragma once

#include <span>
#include <string_view>

#include "aeronet/http-header.hpp"
#include "aeronet/http-status-code.hpp"
#include "raw-chars.hpp"

namespace aeronet {

void BuildSimpleError(http::StatusCode status, std::span<const http::Header> globalHeaders, std::string_view reason,
                      RawChars &out);

}  // namespace aeronet
