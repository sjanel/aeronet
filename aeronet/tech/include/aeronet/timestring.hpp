#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "aeronet/simple-charconv.hpp"
#include "aeronet/timedef.hpp"

namespace aeronet {

// English abbreviations of the week days from Sunday (the order of std::chrono::weekday::c_encoding()), and of the
// months from January, as HTTP dates spell them (RFC 9110 §5.6.7). Fixed-width: no pointer-array indirection.
inline constexpr char kWeekdayAbbreviations[][3]{
    {'S', 'u', 'n'}, {'M', 'o', 'n'}, {'T', 'u', 'e'}, {'W', 'e', 'd'},
    {'T', 'h', 'u'}, {'F', 'r', 'i'}, {'S', 'a', 't'},
};

inline constexpr char kMonthAbbreviations[][3]{
    {'J', 'a', 'n'}, {'F', 'e', 'b'}, {'M', 'a', 'r'}, {'A', 'p', 'r'}, {'M', 'a', 'y'}, {'J', 'u', 'n'},
    {'J', 'u', 'l'}, {'A', 'u', 'g'}, {'S', 'e', 'p'}, {'O', 'c', 't'}, {'N', 'o', 'v'}, {'D', 'e', 'c'},
};

namespace detail {

// The 3 chars at 'ptr' in one integer, to compare a 3-letter abbreviation at once.
constexpr std::uint32_t Pack3(const char* ptr) noexcept {
  return static_cast<std::uint32_t>(static_cast<unsigned char>(ptr[0])) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(ptr[1])) << 8U) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(ptr[2])) << 16U);
}

// Lookup of N 3-letter abbreviations by a perfect hash: the slot ((packed * Multiplier) >> Shift) of 2^Bits is a
// different one for each abbreviation (see perfect()), so that a lookup costs a multiply, a table load and a compare
// whatever the input, where a linear search costs up to N mispredicted branches for unpredictable dates.
template <std::size_t N, std::uint32_t Multiplier, unsigned Shift, unsigned Bits>
class AbbreviationTable {
 public:
  explicit constexpr AbbreviationTable(const char (&abbreviations)[N][3]) noexcept {
    for (std::uint8_t& idx : _indexOfSlot) {
      idx = N;
    }
    for (std::size_t idx = 0; idx < N; ++idx) {
      _packed[idx] = Pack3(abbreviations[idx]);
      _indexOfSlot[Slot(_packed[idx])] = static_cast<std::uint8_t>(idx);
    }
    _packed[N] = ~std::uint32_t{0};  // matches no 3 chars: the result of an empty slot
  }

  // Whether each abbreviation has a slot of its own, which Multiplier and Shift must ensure.
  [[nodiscard]] consteval bool perfect() const noexcept {
    std::size_t nbUsedSlots = 0;
    for (const std::uint8_t idx : _indexOfSlot) {
      nbUsedSlots += idx != N ? 1U : 0U;
    }
    return nbUsedSlots == N;
  }

  // Index of the abbreviation (case-sensitive) that is the 3 chars at 'ptr', N when they are none.
  [[nodiscard]] constexpr std::size_t index(const char* ptr) const noexcept {
    const std::uint32_t packed = Pack3(ptr);
    const std::uint8_t idx = _indexOfSlot[Slot(packed)];
    return _packed[idx] == packed ? idx : N;
  }

 private:
  static constexpr std::size_t Slot(std::uint32_t packed) noexcept {
    return ((packed * Multiplier) >> Shift) & ((1U << Bits) - 1U);
  }

  std::uint32_t _packed[N + 1]{};
  std::uint8_t _indexOfSlot[1U << Bits]{};
};

inline constexpr AbbreviationTable<7, 62, 20, 3> kWeekdayTable(kWeekdayAbbreviations);
inline constexpr AbbreviationTable<12, 122, 16, 4> kMonthTable(kMonthAbbreviations);

static_assert(kWeekdayTable.perfect() && kMonthTable.perfect());

}  // namespace detail

/// The day of the week, as std::chrono::weekday::c_encoding() (0 for Sunday), whose abbreviation ("Sun" to "Sat",
/// case-sensitive) is the 3 chars at 'ptr'. 7 when there is none.
constexpr unsigned WeekdayOfAbbreviation(const char* ptr) noexcept {
  return static_cast<unsigned>(detail::kWeekdayTable.index(ptr));
}

/// The month (1 to 12) whose abbreviation ("Jan" to "Dec", case-sensitive) is the 3 chars at 'ptr', 0 when there is
/// none.
constexpr unsigned MonthOfAbbreviation(const char* ptr) noexcept {
  const std::size_t idx = detail::kMonthTable.index(ptr);
  return idx == 12U ? 0U : static_cast<unsigned>(idx) + 1U;
}

/// Writes chars of the representation of a given time point in ISO 8601 UTC format with maximum performance and return
/// a pointer after the last char written. The written format will be:
///   - 'YYYY-MM-DD'
/// The buffer should have a space of at least 10 chars.
constexpr auto DateISO8601UTC(SysTimePoint timePoint, auto out) {
  const auto daysFloor = std::chrono::floor<std::chrono::days>(timePoint);
  const std::chrono::year_month_day ymd{daysFloor};
  out = write4(out, static_cast<int>(ymd.year()));
  *out = '-';
  out = write2(++out, static_cast<unsigned>(ymd.month()));
  *out = '-';
  return write2(++out, static_cast<unsigned>(ymd.day()));
}

/// Writes chars of the representation of a given time point in ISO 8601 UTC format with maximum performance and return
/// a pointer after the last char written. The written format will be:
///   - 'YYYY-MM-DDTHH:MM:SSZ'
/// The buffer should have a space of at least 20 chars.
template <bool WithFinalZ = true>
constexpr auto TimeToStringISO8601UTC(SysTimePoint timePoint, auto out) {
  const auto daysFloor = std::chrono::floor<std::chrono::days>(timePoint);
  const std::chrono::hh_mm_ss hms{
      std::chrono::floor<std::chrono::milliseconds>(timePoint - daysFloor)};  // ms floor also used by ms variant
  out = DateISO8601UTC(timePoint, out);
  *out = 'T';
  out = write2(++out, hms.hours().count());
  *out = ':';
  out = write2(++out, hms.minutes().count());
  *out = ':';
  out = write2(++out, hms.seconds().count());
  if constexpr (WithFinalZ) {
    *out = 'Z';
    ++out;
  }
  return out;
}

/// Writes chars of the representation of a given time point in ISO 8601 UTC format with maximum performance and return
/// a pointer after the last char written. The written format will be (in millisecond precision):
///   - 'YYYY-MM-DDTHH:MM:SS.sssZ'
/// The buffer should have a space of at least 24 chars (ISO8601UTCWithMsStrLen).
constexpr auto TimeToStringISO8601UTCWithMs(SysTimePoint timePoint, auto out) {
  const auto daysFloor = std::chrono::floor<std::chrono::days>(timePoint);
  const std::chrono::hh_mm_ss hms{std::chrono::floor<std::chrono::milliseconds>(timePoint - daysFloor)};
  out = TimeToStringISO8601UTC<false>(timePoint, out);
  *out = '.';
  out = write3(++out, hms.subseconds().count());
  *out = 'Z';
  return ++out;
}

/// Format a time point to an RFC7231 IMF-fixdate string (e.g. "Sun, 06 Nov 1994 08:49:37 GMT").
/// Buffer must have space for at least 29 characters (no null terminator added):
/// WWW, DD Mon YYYY HH:MM:SS GMT
/// Returns pointer past last written char.
constexpr auto TimeToStringRFC7231(SysTimePoint tp, auto out) {
  static constexpr char kSkeleton[]{
      '?', '?', '?', ',', ' ', '?', '?', ' ', '?', '?', '?', ' ', '?', '?', '?',
      '?', ' ', '?', '?', ':', '?', '?', ':', '?', '?', ' ', 'G', 'M', 'T',
  };

  using namespace std::chrono;

  const sys_seconds secTp = time_point_cast<seconds>(tp);
  const auto day_point = floor<days>(secTp);
  const year_month_day ymd{day_point};
  const weekday wd{day_point};

  // Seconds since midnight, in [0, 86400). Two divisions instead of three:
  // m and s are both derived from the remainder after removing hours.
  const unsigned tod = static_cast<unsigned>((secTp - day_point).count());
  const unsigned hour = tod / 3600;
  const unsigned rem = tod - (hour * 3600);
  const unsigned min = rem / 60;
  const unsigned sec = rem - (min * 60);

  std::memcpy(out, kSkeleton, sizeof(kSkeleton));

  std::memcpy(out, kWeekdayAbbreviations[wd.c_encoding()], sizeof(kWeekdayAbbreviations[0]));
  write2(out + 5, static_cast<unsigned>(ymd.day()));

  std::memcpy(out + 8, kMonthAbbreviations[static_cast<unsigned>(ymd.month()) - 1], sizeof(kMonthAbbreviations[0]));

  write4(out + 12, static_cast<int>(ymd.year()));
  write2(out + 17, hour);
  write2(out + 20, min);
  write2(out + 23, sec);

  return out + sizeof(kSkeleton);
}

// Parse a string representation of a given time point in RFC7231 IMF-fixdate format with maximum performance and
// return a time_point. If parsing fails, returns kInvalidTimePoint.
SysTimePoint TryParseTimeRFC7231(const char* begPtr, const char* endPtr);

// Parse a string representation of a given time point in RFC7231 IMF-fixdate format with maximum performance and
// return a time_point. If parsing fails, returns kInvalidTimePoint.
inline SysTimePoint TryParseTimeRFC7231(std::string_view value) {
  return TryParseTimeRFC7231(value.data(), value.data() + value.size());
}

}  // namespace aeronet
