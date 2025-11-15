#pragma once

#include <functional>
#include <memory>
#include <utility>

#include "aeronet/internal/bytell_hash_map.hpp"

namespace aeronet {

template <typename K, typename V, typename H = std::hash<K>, typename E = std::equal_to<K>,
          typename A = std::allocator<std::pair<K, V> > >
using flat_hash_map = ska::bytell_hash_map<K, V, H, E, A>;

}  // namespace aeronet