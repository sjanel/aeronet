#pragma once

#include <cstddef>
#include <string_view>

namespace aeronet {

namespace detail {

// Sentinel to track a size data in a std::string_view when only the size is relevant (data is not used).
inline constexpr char kSvSizeOnlySentinel{};

}  // namespace detail

constexpr std::string_view SvSizeOnlyView(std::size_t size) noexcept { return {&detail::kSvSizeOnlySentinel, size}; }

constexpr bool IsSvSizeOnlyView(std::string_view sv) noexcept { return sv.data() == &detail::kSvSizeOnlySentinel; }

}  // namespace aeronet
