#pragma once

#include <cstdint>
#include <format>
#include <utility>

#include "timedef.hpp"
#include "timestring.hpp"

namespace aeronet {

struct TimePointISO8601UTC {
  TimePoint tp;
};

}  // namespace aeronet

template <>
struct std::formatter<::aeronet::TimePointISO8601UTC> {
  ///  - '{}' -> Iso8601 without fractional seconds. For instance: "2025-08-14T12:34:56Z"
  ///  - '{:d}' -> Date only. For instance: "2025-08-14"
  ///  - '{:ms}' -> Iso8061 with milliseconds. For instance: "2025-08-14T12:34:56.789Z"
  enum class FormatType : int8_t { Iso8601, DateOnly, Iso8601WithMs };

  FormatType formatType = FormatType::Iso8601;

  constexpr auto parse(format_parse_context &ctx) -> decltype(ctx.begin()) {
    auto it = ctx.begin();
    auto end = ctx.end();
    // Empty spec: leave default and return begin (library expects iterator pointing to '}' in outer context)
    if (it == end || *it == '}') {
      return it;
    }
    if (*it == 'd') {
      formatType = FormatType::DateOnly;
      ++it;
    } else if (*it == 'm') {
      ++it;
      if (it == end || *it != 's') {
        throw format_error("invalid format");
      }
      ++it;
      formatType = FormatType::Iso8601WithMs;
    } else {
      throw format_error("invalid format");
    }
    // After consuming our spec, next char must be '}' or end (end indicates library will report error anyway)
    if (it != end && *it != '}') {
      throw format_error("invalid format");
    }
    return it;  // position right before '}' which lib will validate/consume.
  }

  template <typename FormatContext>
  auto format(::aeronet::TimePointISO8601UTC tp, FormatContext &ctx) const -> decltype(ctx.out()) {
    switch (formatType) {
      case FormatType::Iso8601:
        ctx.out() = aeronet::TimeToStringISO8601UTC(tp.tp, ctx.out());
        break;
      case FormatType::DateOnly:
        ctx.out() = aeronet::DateISO8601UTC(tp.tp, ctx.out());
        break;
      case FormatType::Iso8601WithMs:
        ctx.out() = aeronet::TimeToStringISO8601UTCWithMs(tp.tp, ctx.out());
        break;
      default:
        std::unreachable();
    }
    return ctx.out();
  }
};
