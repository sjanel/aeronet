#pragma once

#include <functional>

#include "aeronet/internal/bytell_hash_map.hpp"

namespace aeronet {

template <typename K, typename V, typename H = std::hash<K>, typename E = std::equal_to<K> >
using flat_hash_map = ska::bytell_hash_map<K, V, H, E>;

}  // namespace aeronet