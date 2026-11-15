#pragma once

#include <cassert>
#include <cstdint>
#include <glaze/glaze.hpp>
#include <string>
#include <string_view>

#include "aeronet/major-minor-version.hpp"

// --- MajorMinorVersion<Prefix>: read/write as string like "1.2" ---
template <const char* Prefix>
struct glz::meta<aeronet::MajorMinorVersion<Prefix>> {
  using T = aeronet::MajorMinorVersion<Prefix>;
  static constexpr bool custom_read = true;
  static constexpr bool custom_write = true;
};

template <uint32_t Format, const char* Prefix>
struct glz::from<Format, aeronet::MajorMinorVersion<Prefix>> {
  template <auto Opts>
  static void op(aeronet::MajorMinorVersion<Prefix>& value, is_context auto&& ctx, auto&& it, auto&& end) {
    std::string str;
    from<Format, std::string>::template op<Opts>(str, ctx, it, end);
    assert(!bool(ctx.error));  // Glaze validated structure before calling custom reader
    if (str.empty()) {
      value = aeronet::MajorMinorVersion<Prefix>{};
      return;
    }
    // Accept "X.Y" (short form) or "PrefixX.Y" (full form)
    static constexpr std::string_view prefix = Prefix;
    if (str.size() == 3 && str[1] == '.') {
      // Short form "X.Y" - construct by prepending the prefix
      std::string full{prefix};
      full += str;
      value = aeronet::MajorMinorVersion<Prefix>{std::string_view{full}};
    } else {
      value = aeronet::MajorMinorVersion<Prefix>{std::string_view{str}};
    }
    if (!value.isValid() && !str.empty()) {
      ctx.error = error_code::parse_error;
    }
  }
};

template <uint32_t Format, const char* Prefix>
struct glz::to<Format, aeronet::MajorMinorVersion<Prefix>> {
  template <auto Opts, is_context Ctx, class B, class IX>
  static void op(const aeronet::MajorMinorVersion<Prefix>& value, Ctx&& ctx, B&& buffer, IX&& ix) {
    if (value.isValid()) {
      // Write as short form "X.Y"
      const char buf[]{static_cast<char>('0' + value.major()), '.', static_cast<char>('0' + value.minor())};
      serialize<Format>::template op<Opts>(std::string_view{buf, sizeof(buf)}, ctx, buffer, ix);
    } else {
      serialize<Format>::template op<Opts>(std::string_view{""}, ctx, buffer, ix);
    }
  }
};
