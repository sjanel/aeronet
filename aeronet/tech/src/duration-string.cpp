#include "aeronet/duration-string.hpp"

#include <algorithm>
#include <cassert>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>

#include "aeronet/cctype.hpp"
#include "aeronet/log.hpp"
#include "aeronet/memory-utils.hpp"
#include "aeronet/nchars.hpp"

namespace aeronet {

namespace {
using UnitDuration = std::pair<std::string_view, std::chrono::milliseconds>;

constexpr UnitDuration kDurationUnits[] = {
    {"y", std::chrono::years(1)},   {"mon", std::chrono::months(1)}, {"w", std::chrono::weeks(1)},
    {"d", std::chrono::days(1)},    {"d", std::chrono::days(1)},     {"h", std::chrono::hours(1)},
    {"m", std::chrono::minutes(1)}, {"s", std::chrono::seconds(1)},  {"ms", std::chrono::milliseconds(1)},
};

constexpr char kInvalidTimeDurationUnitMsg[] =
    "Cannot parse time duration '{}'. Accepted time units are 'y, mon, w, d, h, m (minutes), s and ms'";

constexpr auto AdvanceSpaces(const char* ptr, const char* end) {
  while (ptr < end && isspace(*ptr)) {
    ++ptr;
  }
  return ptr;
}

}  // namespace

std::string_view::size_type DurationLen(std::string_view str) {
  std::string_view::size_type ret{};

  bool negativeSignAllowed = true;

  const char* beg = str.data();
  const char* end = str.data() + str.size();
  while (beg < end) {
    beg = AdvanceSpaces(beg, end);

    int64_t value;
    const auto [ptr, err] = std::from_chars(beg, end, value);
    if (err != std::errc()) {
      break;
    }

    if (value < 0) {
      if (negativeSignAllowed) {
        negativeSignAllowed = false;
      } else {
        break;  // Several negative values are not allowed
      }
    }

    beg = ptr;

    beg = AdvanceSpaces(beg, end);
    const auto unitFirst = beg;
    while (beg < end && islower(*beg)) {
      ++beg;
    }
    const auto it = std::ranges::find_if(kDurationUnits, [unitFirst, beg](const auto& durationUnitWithDuration) {
      return durationUnitWithDuration.first.size() == static_cast<std::string_view::size_type>(beg - unitFirst) &&
             std::memcmp(unitFirst, durationUnitWithDuration.first.data(), static_cast<std::size_t>(beg - unitFirst)) ==
                 0;
    });
    if (it == std::end(kDurationUnits)) {
      break;
    }

    // There is a substring with size 'charPos' that represents a duration
    ret = static_cast<std::string_view::size_type>(beg - str.data());
  }

  return ret;
}

std::chrono::milliseconds ParseDuration(std::string_view durationStr) {
  const char* beg = durationStr.data();
  const char* end = durationStr.data() + durationStr.size();

  beg = AdvanceSpaces(beg, end);

  const bool isNegative = *beg == '-';
  if (isNegative) {
    beg = AdvanceSpaces(++beg, end);
  }

  if (beg == end) {
    log::critical(kInvalidTimeDurationUnitMsg, durationStr);
    throw std::invalid_argument("Cannot parse time duration");
  }

  std::chrono::milliseconds ret{};
  while (beg < end) {
    const auto intFirst = beg;

    while (beg < end && isdigit(*beg)) {
      ++beg;
    }
    if (intFirst == beg) {
      log::critical(kInvalidTimeDurationUnitMsg, durationStr);
      throw std::invalid_argument("Cannot parse time duration");
    }

    int64_t timeAmount;
    [[maybe_unused]] const auto [ptr, err] = std::from_chars(intFirst, beg, timeAmount);
    assert(err == std::errc() && ptr == beg);

    beg = AdvanceSpaces(beg, end);
    const auto unitFirst = beg;
    while (beg < end && islower(*beg)) {
      ++beg;
    }
    const auto it = std::ranges::find_if(kDurationUnits, [unitFirst, beg](const auto& durationUnitWithDuration) {
      return durationUnitWithDuration.first.size() == static_cast<std::string_view::size_type>(beg - unitFirst) &&
             std::memcmp(unitFirst, durationUnitWithDuration.first.data(), static_cast<std::size_t>(beg - unitFirst)) ==
                 0;
    });
    if (it == std::end(kDurationUnits)) {
      log::critical(kInvalidTimeDurationUnitMsg, durationStr);
      throw std::invalid_argument("Cannot parse time duration");
    }
    ret += timeAmount * it->second;
    beg = AdvanceSpaces(beg, end);
  }

  return isNegative ? -ret : ret;
}

namespace {

bool AdjustWithUnit(UnitDuration unitDuration, std::chrono::milliseconds& dur, int& nbSignificantUnits,
                    RawChars32& ret) {
  if (dur >= unitDuration.second) {
    const auto countInThisDurationUnit =
        std::chrono::duration_cast<decltype(unitDuration.second)>(dur).count() / unitDuration.second.count();
    ret.ensureAvailableCapacityExponential(nchars(countInThisDurationUnit) + unitDuration.first.size());

    auto ptr = std::to_chars(ret.data() + ret.size(), ret.data() + ret.capacity(), countInThisDurationUnit).ptr;
    ptr = Append(unitDuration.first, ptr);
    ret.setSize(static_cast<uint32_t>(ptr - ret.data()));

    dur -= countInThisDurationUnit * unitDuration.second;
    if (--nbSignificantUnits == 0) {
      return true;
    }
  }
  return false;
}

}  // namespace

RawChars32 DurationToString(std::chrono::milliseconds dur, int nbSignificantUnits) {
  RawChars32 ret;

  if (dur == std::chrono::milliseconds{0}) {
    ret.append("0ms");
  } else {
    if (dur < std::chrono::milliseconds{0}) {
      ret.push_back('-');
      dur = -dur;
    }

    std::ranges::find_if(kDurationUnits, [&dur, &nbSignificantUnits, &ret](const auto& unitDuration) {
      return AdjustWithUnit(unitDuration, dur, nbSignificantUnits, ret);
    });
  }

  return ret;
}

std::string_view::size_type DurationLen(std::chrono::milliseconds dur, int nbSignificantUnits) {
  std::string_view::size_type ret = 0;
  if (dur < std::chrono::milliseconds{0}) {
    ret += 1;
    dur = -dur;
  }

  std::ranges::find_if(kDurationUnits, [&dur, &nbSignificantUnits, &ret](const auto& unitDuration) {
    if (dur >= unitDuration.second) {
      const auto countInThisDurationUnit =
          std::chrono::duration_cast<decltype(unitDuration.second)>(dur).count() / unitDuration.second.count();
      ret += nchars(countInThisDurationUnit);
      ret += unitDuration.first.size();
      dur -= countInThisDurationUnit * unitDuration.second;
      if (--nbSignificantUnits == 0) {
        return true;
      }
    }
    return false;
  });

  return ret;
}

}  // namespace aeronet
