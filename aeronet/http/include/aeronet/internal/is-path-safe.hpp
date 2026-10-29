#pragma once

#include <string_view>

namespace aeronet::internal {

constexpr bool IsUnreserved(char ch) noexcept {
  return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '.' ||
         ch == '_' || ch == '~';
}

// Characters allowed verbatim in a URL path: unreserved + path sub-delims + ":" / "@" / "/".
// '%' is intentionally excluded so a decoded literal '%' is re-encoded to "%25".
// Backed by a compile-time 256-entry lookup table so the per-char test (run twice over the path:
// once to size, once to encode) is a single load instead of a branch chain.
constexpr bool IsPathSafe(char ch) noexcept {
  static constexpr auto kPathSafeTable = [] {
    struct {
      bool data[256];
    } table{};
    constexpr std::string_view kPathExtra = "/:@!$&'()*+,;=";
    for (int ic = 0; ic < 256; ++ic) {
      const char cc = static_cast<char>(ic);
      table.data[ic] = IsUnreserved(cc) || kPathExtra.contains(cc);
    }
    return table;
  }();
  return kPathSafeTable.data[static_cast<unsigned char>(ch)];
}

}  // namespace aeronet::internal
