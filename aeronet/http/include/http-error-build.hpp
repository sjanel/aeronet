#pragma once

#include <span>
#include <string_view>

#include "aeronet/http-header.hpp"
#include "aeronet/http-response-data.hpp"
#include "aeronet/http-status-code.hpp"

namespace aeronet {

HttpResponseData BuildSimpleError(http::StatusCode status, std::span<const http::Header> globalHeaders,
                                  std::string_view reason);

}  // namespace aeronet
