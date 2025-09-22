#pragma once

#include <string_view>

#include "http-status-code.hpp"
#include "raw-chars.hpp"

namespace aeronet::http {

RawChars buildStatusLine(http::StatusCode code, std::string_view reason);

}