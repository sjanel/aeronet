#pragma once

#include <string_view>

#include "aeronet/city-hash.hpp"
#include "aeronet/flat-hash-map.hpp"

namespace aeronet {

using SvToSvMap = flat_hash_map<std::string_view, std::string_view, CityHash>;

}