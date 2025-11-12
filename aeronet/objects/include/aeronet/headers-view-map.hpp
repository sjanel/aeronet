#pragma once

#include <string_view>

#include "aeronet/flat-hash-map.hpp"
#include "aeronet/string-equal-ignore-case.hpp"

namespace aeronet {

using HeadersViewMap =
    flat_hash_map<std::string_view, std::string_view, CaseInsensitiveHashFunc, CaseInsensitiveEqualFunc>;

}