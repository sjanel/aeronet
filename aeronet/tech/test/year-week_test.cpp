#include "year-week.hpp"

#include <gtest/gtest.h>

#include "timestring.hpp"

namespace aeronet {

TEST(IsWeekNumberTest, RealCases) {
  EXPECT_EQ(IsoWeekNumber(StringToTimeISO8601UTC("2005-01-01")), 53);
  EXPECT_EQ(IsoWeekNumber(StringToTimeISO8601UTC("2005-01-02")), 53);
  EXPECT_EQ(IsoWeekNumber(StringToTimeISO8601UTC("2005-12-31")), 52);
  EXPECT_EQ(IsoWeekNumber(StringToTimeISO8601UTC("2006-01-01")), 52);
  EXPECT_EQ(IsoWeekNumber(StringToTimeISO8601UTC("2006-01-02")), 1);
  EXPECT_EQ(IsoWeekNumber(StringToTimeISO8601UTC("2006-12-31")), 52);
  EXPECT_EQ(IsoWeekNumber(StringToTimeISO8601UTC("2007-01-01")), 1);
  EXPECT_EQ(IsoWeekNumber(StringToTimeISO8601UTC("2007-12-30")), 52);
  EXPECT_EQ(IsoWeekNumber(StringToTimeISO8601UTC("2007-12-31")), 1);
  EXPECT_EQ(IsoWeekNumber(StringToTimeISO8601UTC("2008-01-01")), 1);
  EXPECT_EQ(IsoWeekNumber(StringToTimeISO8601UTC("2008-12-28")), 52);
  EXPECT_EQ(IsoWeekNumber(StringToTimeISO8601UTC("2008-12-29")), 1);
  EXPECT_EQ(IsoWeekNumber(StringToTimeISO8601UTC("2008-12-30")), 1);
  EXPECT_EQ(IsoWeekNumber(StringToTimeISO8601UTC("2008-12-31")), 1);
  EXPECT_EQ(IsoWeekNumber(StringToTimeISO8601UTC("2009-01-01")), 1);
  EXPECT_EQ(IsoWeekNumber(StringToTimeISO8601UTC("2010-01-01")), 53);
  EXPECT_EQ(IsoWeekNumber(StringToTimeISO8601UTC("2010-01-02")), 53);
  EXPECT_EQ(IsoWeekNumber(StringToTimeISO8601UTC("2010-01-03")), 53);

  EXPECT_EQ(IsoWeekNumber(StringToTimeISO8601UTC("2025-08-03")), 31);
  EXPECT_EQ(IsoWeekNumber(StringToTimeISO8601UTC("2025-08-04")), 32);
}

}  // namespace aeronet
