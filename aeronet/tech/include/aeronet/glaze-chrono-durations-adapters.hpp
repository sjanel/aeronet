#pragma once

#include <cassert>
#include <chrono>
#include <cstdint>
#include <glaze/glaze.hpp>
#include <glaze/yaml.hpp>  // IWYU pragma: keep
#include <stdexcept>
#include <string>
#include <string_view>

#include "aeronet/duration-string.hpp"

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
        value = aeronet::ParseDuration(str);
      } catch ([[maybe_unused]] const std::invalid_argument& ex) {
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
      value = aeronet::ParseDuration(str);
    } catch ([[maybe_unused]] const std::invalid_argument& ex) {
      ctx.error = error_code::parse_error;
    }
  }
};

// Write milliseconds as human-readable string
template <>
struct to<JSON, std::chrono::milliseconds> {
  template <auto Opts, is_context Ctx, class B, class IX>
  static void op(const std::chrono::milliseconds& value, Ctx&& ctx, B&& b, IX&& ix) {
    auto str = aeronet::DurationToString(value);
    serialize<JSON>::op<Opts>(std::string_view(str), ctx, b, ix);
  }
};

template <>
struct to<YAML, std::chrono::milliseconds> {
  template <auto Opts, is_context Ctx, class B, class IX>
  static void op(const std::chrono::milliseconds& value, Ctx&& ctx, B&& b, IX&& ix) {
    auto str = aeronet::DurationToString(value);
    serialize<YAML>::op<Opts>(std::string_view(str), ctx, b, ix);
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
        value = std::chrono::duration_cast<std::chrono::seconds>(aeronet::ParseDuration(str));
      } catch ([[maybe_unused]] const std::invalid_argument& ex) {
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
      value = std::chrono::duration_cast<std::chrono::seconds>(aeronet::ParseDuration(str));
    } catch ([[maybe_unused]] const std::invalid_argument& ex) {
      ctx.error = error_code::parse_error;
    }
  }
};

template <>
struct to<JSON, std::chrono::seconds> {
  template <auto Opts, is_context Ctx, class B, class IX>
  static void op(const std::chrono::seconds& value, Ctx&& ctx, B&& b, IX&& ix) {
    auto str = aeronet::DurationToString(std::chrono::duration_cast<std::chrono::milliseconds>(value));
    serialize<JSON>::op<Opts>(std::string_view(str), ctx, b, ix);
  }
};

template <>
struct to<YAML, std::chrono::seconds> {
  template <auto Opts, is_context Ctx, class B, class IX>
  static void op(const std::chrono::seconds& value, Ctx&& ctx, B&& b, IX&& ix) {
    auto str = aeronet::DurationToString(std::chrono::duration_cast<std::chrono::milliseconds>(value));
    serialize<YAML>::op<Opts>(std::string_view(str), ctx, b, ix);
  }
};

}  // namespace glz
