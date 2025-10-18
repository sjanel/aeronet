#pragma once

#include <chrono>
#include <format>
#include <utility>

#include "timedef.hpp"

namespace aeronet {

struct PrettyDuration {
  SysDuration dur;
};

}  // namespace aeronet

template <>
struct std::formatter<::aeronet::PrettyDuration> {
  static constexpr std::pair<std::string_view, ::aeronet::SysDuration> kDurationUnitsFormat[] = {
      {"y", std::chrono::years(1)},         {"d", std::chrono::days(1)},         {"h", std::chrono::hours(1)},
      {"m", std::chrono::minutes(1)},       {"s", std::chrono::seconds(1)},      {"ms", std::chrono::milliseconds(1)},
      {"us", std::chrono::microseconds(1)}, {"ns", std::chrono::nanoseconds(1)},
  };

  ///  - '{}' -> all units
  ///  - '{:[1-8]}' -> n units max'
  int nbUnitsToPrint = std::size(kDurationUnitsFormat);

  constexpr auto parse(format_parse_context &ctx) -> decltype(ctx.begin()) {
    auto it = ctx.begin();
    const auto end = ctx.end();
    if (it == end) {
      return it;  // nothing to do (should not really happen for valid format strings)
    }
    if (*it == '}') {
      // default: all units
      return it;  // must return iterator pointing at '}'
    }
    // Expect a single digit 1..N then a '}'
    const char c = *it;
    if (c < '1' || c > static_cast<char>('0' + std::size(kDurationUnitsFormat))) {
      throw format_error("invalid PrettyDuration spec");
    }
    nbUnitsToPrint = c - '0';
    ++it;
    if (it == end || *it != '}') {
      throw format_error("invalid PrettyDuration spec - missing closing brace");
    }
    return it;  // points at '}'
  }

  template <typename FormatContext>
  auto format(::aeronet::PrettyDuration dur, FormatContext &ctx) const -> decltype(ctx.out()) {
    if (dur.dur < ::aeronet::SysDuration::zero()) {
      ctx.out() = std::format_to(ctx.out(), "-");
      dur.dur = -dur.dur;
    }

    for (int unitPos = 0, unitToPrintPos = 0;
         unitToPrintPos < nbUnitsToPrint && dur.dur > ::aeronet::SysDuration::zero(); ++unitPos) {
      const auto &unitDuration = kDurationUnitsFormat[unitPos];
      if (dur.dur >= unitDuration.second) {
        const auto countInThisDurationUnit =
            std::chrono::duration_cast<decltype(unitDuration.second)>(dur.dur).count() / unitDuration.second.count();
        ctx.out() = std::format_to(ctx.out(), "{}{}", countInThisDurationUnit, unitDuration.first);
        dur.dur -= countInThisDurationUnit * unitDuration.second;
        ++unitToPrintPos;
      }
    }
    return ctx.out();
  }
};
