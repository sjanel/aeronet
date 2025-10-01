#pragma once

#include <string_view>

#include "aeronet/http-status-code.hpp"
#include "raw-chars.hpp"

namespace aeronet::http {

RawChars buildStatusLine(http::StatusCode code, std::string_view reason);

}