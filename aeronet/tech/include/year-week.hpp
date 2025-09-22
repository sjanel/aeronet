#pragma once

#include <chrono>
#include <cstdint>

#include "timedef.hpp"

namespace aeronet {

// Taken from https://stackoverflow.com/questions/77754254/compute-iso-week-based-date-using-c20-chrono
struct iso_week_date {
  constexpr iso_week_date(std::uint16_t year, unsigned wn, std::chrono::weekday wd) noexcept;
  constexpr iso_week_date(std::chrono::sys_days tp) noexcept;
  constexpr operator std::chrono::sys_days() const noexcept;

  std::uint16_t year;
  std::uint8_t weeknum;
  std::chrono::weekday dow;
};

constexpr iso_week_date::iso_week_date(std::uint16_t year, unsigned wn, std::chrono::weekday wd) noexcept
    : year{year}, weeknum(static_cast<std::uint8_t>(wn)), dow{wd} {}

constexpr iso_week_date::iso_week_date(std::chrono::sys_days tp) noexcept
    : iso_week_date{0, 0, std::chrono::weekday{}} {
  using std::chrono::days;
  using std::chrono::floor;
  using std::chrono::Monday;
  using std::chrono::sys_days;
  using std::chrono::Thursday;
  using std::chrono::weekday;
  using std::chrono::weeks;
  using std::chrono::year_month_day;

  dow = weekday{tp};
  auto closest_thursday = [this](sys_days tp) {
    auto dowIso = static_cast<int>(dow.iso_encoding());
    tp += days{4 - dowIso};
    return tp;
  };

  auto ymdYear = year_month_day{closest_thursday(tp)}.year();
  auto start = sys_days{ymdYear / 1 / Thursday[1]} - (Thursday - Monday);
  year = static_cast<std::uint16_t>(int{ymdYear});
  weeknum = static_cast<std::uint8_t>(floor<weeks>(tp - start) / weeks{1} + 1);
}

constexpr iso_week_date::operator std::chrono::sys_days() const noexcept {
  using std::chrono::Monday;
  using std::chrono::sys_days;
  using std::chrono::Thursday;
  using std::chrono::weeks;

  auto start = sys_days{std::chrono::year{year} / 1 / Thursday[1]} - (Thursday - Monday);
  return start + weeks{weeknum - 1} + (dow - Monday);
}

// Week number according to the ISO-8601 standard, weeks starting on Monday.
constexpr int IsoWeekNumber(std::chrono::sys_days sysDays) {
  iso_week_date weekDate{sysDays};
  return static_cast<int>(weekDate.weeknum);
}

// Week number according to the ISO-8601 standard, weeks starting on Monday.
constexpr int IsoWeekNumber(TimePoint tp) { return IsoWeekNumber(std::chrono::floor<std::chrono::days>(tp)); }

}  // namespace aeronet