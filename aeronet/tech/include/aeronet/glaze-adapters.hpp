#pragma once

#ifdef AERONET_ENABLE_GLAZE

#include <array>
#include <cassert>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <glaze/glaze.hpp>
#include <glaze/yaml.hpp>  // IWYU pragma: keep
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>

#include "aeronet/concatenated-headers.hpp"
#include "aeronet/concatenated-strings.hpp"
#include "aeronet/major-minor-version.hpp"
#include "aeronet/static-concatenated-strings.hpp"
#include "aeronet/vector.hpp"

namespace aeronet::detail {

/// Parse a duration string like "5s", "500ms", "1h30m", "200us", or a plain integer (interpreted as milliseconds).
/// Supported units: h (hours), m (minutes), s (seconds), ms (milliseconds), us (microseconds), ns (nanoseconds).
/// Amounts and units may be separated by spaces, e.g. "1h 30m" or "1h30m".
inline std::chrono::milliseconds ParseDurationString(std::string_view str) {
  const char* beg = str.data();
  const char* end = beg + str.size();

  // Skip leading whitespace.
  while (beg < end && (*beg == ' ' || *beg == '\t')) {
    ++beg;
  }
  // Skip trailing whitespace.
  while (beg < end && (*(end - 1) == ' ' || *(end - 1) == '\t')) {
    --end;
  }

  if (beg == end) {
    throw std::invalid_argument("Empty duration string");
  }

  // Try plain integer first (milliseconds).
  {
    int64_t rawMs{};
    auto [ptr, ec] = std::from_chars(beg, end, rawMs);
    if (ec == std::errc() && ptr == end) {
      return std::chrono::milliseconds{rawMs};
    }
  }

  // Parse compound duration string.
  using namespace std::chrono;

  struct UnitEntry {
    std::string_view suffix;
    nanoseconds factor;
  };

  // Order matters: longer suffixes first to avoid "ms" matching "m" then "s".
  static constexpr UnitEntry kUnits[] = {
      {"ms", milliseconds{1}}, {"us", microseconds{1}}, {"ns", nanoseconds{1}},
      {"h", hours{1}},         {"m", minutes{1}},       {"s", seconds{1}},
  };

  nanoseconds total{0};

  while (beg < end) {
    while (beg < end && (*beg == ' ' || *beg == '\t')) {
      ++beg;
    }
    assert(beg < end);  // Unreachable: trailing whitespace was trimmed at function entry

    int64_t amount{};
    auto [numEnd, ec] = std::from_chars(beg, end, amount);
    if (ec != std::errc() || numEnd == beg) {
      throw std::invalid_argument(std::string("Invalid duration string: '") + std::string(str) + "'");
    }
    beg = numEnd;

    while (beg < end && (*beg == ' ' || *beg == '\t')) {
      ++beg;
    }

    // Match unit suffix.
    bool matched = false;
    for (const auto& [suffix, factor] : kUnits) {
      if (static_cast<std::size_t>(end - beg) >= suffix.size() && std::string_view(beg, suffix.size()) == suffix) {
        total += amount * factor;
        beg += suffix.size();
        matched = true;
        break;
      }
    }
    if (!matched) {
      throw std::invalid_argument(std::string("Unknown unit in duration string: '") + std::string(str) + "'");
    }
  }

  return duration_cast<milliseconds>(total);
}

}  // namespace aeronet::detail

// ============================================================================
// Glaze custom adapters for aeronet types
// ============================================================================

// --- std::chrono::milliseconds: read integer (ms) or string ("5s", "500ms"), write integer ---
template <>
struct glz::meta<std::chrono::milliseconds> {
  using T = std::chrono::milliseconds;
  static constexpr bool custom_read = true;
  static constexpr bool custom_write = true;
};

// --- std::chrono::seconds: read integer (seconds) or string, write integer ---
template <>
struct glz::meta<std::chrono::seconds> {
  using T = std::chrono::seconds;
  static constexpr bool custom_read = true;
  static constexpr bool custom_write = true;
};

namespace glz {

template <>
struct from<JSON, std::chrono::milliseconds> {
  template <auto Opts>
  static void op(std::chrono::milliseconds& value, is_context auto&& ctx, auto&& it, auto&& end) {
    // Glaze already consumed JSON whitespace before calling custom reader
    auto start = it;
    assert(it >= end || (*it != ' ' && *it != '\t' && *it != '\n' && *it != '\r'));
    if (it < end && *it == '"') {
      // Read as string, then parse duration
      std::string str;
      parse<JSON>::op<Opts>(str, ctx, it, end);
      assert(!bool(ctx.error));  // Glaze validated JSON structure before calling custom reader
      try {
        value = aeronet::detail::ParseDurationString(str);
      } catch (const std::invalid_argument& ex) {
        ctx.error = error_code::parse_error;
      }
    } else {
      // Read as integer (milliseconds)
      it = start;
      int64_t ms{};
      parse<JSON>::op<Opts>(ms, ctx, it, end);
      if (!bool(ctx.error)) {
        value = std::chrono::milliseconds{ms};
      }
    }
  }
};

template <>
struct from<YAML, std::chrono::milliseconds> {
  template <auto Opts>
  static void op(std::chrono::milliseconds& value, is_context auto&& ctx, auto&& it, auto&& end) {
    // In YAML, values are unquoted. Read as string, attempt integer then duration parse.
    std::string str;
    from<YAML, std::string>::template op<Opts>(str, ctx, it, end);
    assert(!bool(ctx.error));  // Glaze validated YAML structure before calling custom reader
    try {
      value = aeronet::detail::ParseDurationString(str);
    } catch (const std::invalid_argument&) {
      ctx.error = error_code::parse_error;
    }
  }
};

// Write milliseconds as integer
template <>
struct to<JSON, std::chrono::milliseconds> {
  template <auto Opts, is_context Ctx, class B, class IX>
  static void op(const std::chrono::milliseconds& value, Ctx&& ctx, B&& b, IX&& ix) {
    serialize<JSON>::op<Opts>(value.count(), ctx, b, ix);
  }
};

template <>
struct to<YAML, std::chrono::milliseconds> {
  template <auto Opts, is_context Ctx, class B, class IX>
  static void op(const std::chrono::milliseconds& value, Ctx&& ctx, B&& b, IX&& ix) {
    serialize<YAML>::op<Opts>(value.count(), ctx, b, ix);
  }
};

// --- std::chrono::seconds: read integer (seconds) or string, write integer ---
template <>
struct from<JSON, std::chrono::seconds> {
  template <auto Opts>
  static void op(std::chrono::seconds& value, is_context auto&& ctx, auto&& it, auto&& end) {
    // Glaze already consumed JSON whitespace before calling custom reader
    auto start = it;
    assert(it >= end || (*it != ' ' && *it != '\t' && *it != '\n' && *it != '\r'));
    if (it < end && *it == '"') {
      std::string str;
      parse<JSON>::op<Opts>(str, ctx, it, end);
      assert(!bool(ctx.error));  // Glaze validated JSON structure before calling custom reader
      try {
        value = std::chrono::duration_cast<std::chrono::seconds>(aeronet::detail::ParseDurationString(str));
      } catch (const std::invalid_argument&) {
        ctx.error = error_code::parse_error;
      }
    } else {
      it = start;
      int64_t sec{};
      parse<JSON>::op<Opts>(sec, ctx, it, end);
      if (!bool(ctx.error)) {
        value = std::chrono::seconds{sec};
      }
    }
  }
};

template <>
struct from<YAML, std::chrono::seconds> {
  template <auto Opts>
  static void op(std::chrono::seconds& value, is_context auto&& ctx, auto&& it, auto&& end) {
    std::string str;
    from<YAML, std::string>::template op<Opts>(str, ctx, it, end);
    assert(!bool(ctx.error));  // Glaze validated YAML structure before calling custom reader
    try {
      value = std::chrono::duration_cast<std::chrono::seconds>(aeronet::detail::ParseDurationString(str));
    } catch (const std::invalid_argument&) {
      ctx.error = error_code::parse_error;
    }
  }
};

template <>
struct to<JSON, std::chrono::seconds> {
  template <auto Opts, is_context Ctx, class B, class IX>
  static void op(const std::chrono::seconds& value, Ctx&& ctx, B&& b, IX&& ix) {
    serialize<JSON>::op<Opts>(value.count(), ctx, b, ix);
  }
};

template <>
struct to<YAML, std::chrono::seconds> {
  template <auto Opts, is_context Ctx, class B, class IX>
  static void op(const std::chrono::seconds& value, Ctx&& ctx, B&& b, IX&& ix) {
    serialize<YAML>::op<Opts>(value.count(), ctx, b, ix);
  }
};

// --- DynamicConcatenatedStrings<Sep, SizeType>: read/write as array of strings ---
template <const char* Sep, class SizeType>
struct meta<aeronet::DynamicConcatenatedStrings<Sep, SizeType>> {
  static constexpr bool custom_read = true;
  static constexpr bool custom_write = true;
};

template <uint32_t Format, const char* Sep, class SizeType>
struct from<Format, aeronet::DynamicConcatenatedStrings<Sep, SizeType>> {
  template <auto Opts>
  static void op(aeronet::DynamicConcatenatedStrings<Sep, SizeType>& value, is_context auto&& ctx, auto&& it,
                 auto&& end) {
    aeronet::vector<std::string> arr;
    from<Format, aeronet::vector<std::string>>::template op<Opts>(arr, ctx, it, end);
    assert(!bool(ctx.error));  // Glaze validated structure before calling custom reader
    value.clear();
    for (const auto& str : arr) {
      value.append(str);
    }
  }
};

template <uint32_t Format, const char* Sep, class SizeType>
struct to<Format, aeronet::DynamicConcatenatedStrings<Sep, SizeType>> {
  template <auto Opts, is_context Ctx, class B, class IX>
  static void op(const aeronet::DynamicConcatenatedStrings<Sep, SizeType>& value, Ctx&& ctx, B&& b, IX&& ix) {
    aeronet::vector<std::string_view> arr;
    for (auto sv : value) {
      arr.push_back(sv);
    }
    serialize<Format>::template op<Opts>(arr, ctx, b, ix);
  }
};

// --- StaticConcatenatedStrings<N, SizeType>: read/write as array of strings ---
template <unsigned N, class SizeType>
struct meta<aeronet::StaticConcatenatedStrings<N, SizeType>> {
  static constexpr bool custom_read = true;
  static constexpr bool custom_write = true;
};

template <uint32_t Format, unsigned N, class SizeType>
struct from<Format, aeronet::StaticConcatenatedStrings<N, SizeType>> {
  template <auto Opts>
  static void op(aeronet::StaticConcatenatedStrings<N, SizeType>& value, is_context auto&& ctx, auto&& it, auto&& end) {
    std::array<std::string, N> arr;
    from<Format, std::array<std::string, N>>::template op<Opts>(arr, ctx, it, end);
    assert(!bool(ctx.error));  // Glaze validated structure before calling custom reader
    for (unsigned idx = 0; idx < N; ++idx) {
      value.set(idx, arr[idx]);
    }
  }
};

template <uint32_t Format, unsigned N, class SizeType>
struct to<Format, aeronet::StaticConcatenatedStrings<N, SizeType>> {
  template <auto Opts, is_context Ctx, class B, class IX>
  static void op(const aeronet::StaticConcatenatedStrings<N, SizeType>& value, Ctx&& ctx, B&& b, IX&& ix) {
    std::array<std::string_view, N> arr;
    for (unsigned idx = 0; idx < N; ++idx) {
      arr[idx] = value[idx];
    }
    serialize<Format>::template op<Opts>(arr, ctx, b, ix);
  }
};

// --- MajorMinorVersion<Prefix>: read/write as string like "1.2" ---
template <const char* Prefix>
struct meta<aeronet::MajorMinorVersion<Prefix>> {
  using T = aeronet::MajorMinorVersion<Prefix>;
  static constexpr bool custom_read = true;
  static constexpr bool custom_write = true;
};

template <uint32_t Format, const char* Prefix>
struct from<Format, aeronet::MajorMinorVersion<Prefix>> {
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
    constexpr std::string_view prefix = Prefix;
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
struct to<Format, aeronet::MajorMinorVersion<Prefix>> {
  template <auto Opts, is_context Ctx, class B, class IX>
  static void op(const aeronet::MajorMinorVersion<Prefix>& value, Ctx&& ctx, B&& b, IX&& ix) {
    if (!value.isValid()) {
      serialize<Format>::template op<Opts>(std::string_view{""}, ctx, b, ix);
    } else {
      // Write as short form "X.Y"
      char buf[4];
      buf[0] = static_cast<char>('0' + value.major());
      buf[1] = '.';
      buf[2] = static_cast<char>('0' + value.minor());
      buf[3] = '\0';
      serialize<Format>::template op<Opts>(std::string_view{buf, 3}, ctx, b, ix);
    }
  }
};

}  // namespace glz

#endif  // AERONET_ENABLE_GLAZE
