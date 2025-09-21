#pragma once

#include <string_view>

#include "raw-bytes.hpp"

namespace aeronet {

using RawChars = RawBytes<char, std::string_view>;

}