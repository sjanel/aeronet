#include "exception.hpp"

#include <gtest/gtest.h>

namespace aeronet {

TEST(ExceptionTest, InfoTakenFromConstCharStar) {
  EXPECT_STREQ(exception("This string can fill the inline storage").what(), "This string can fill the inline storage");
}

TEST(ExceptionTest, FormatUntruncated) {
  EXPECT_STREQ(exception("There are so many {} in the world, I counted {}", "routes", 42).what(),
               "There are so many routes in the world, I counted 42");
}

TEST(ExceptionTest, FormatTruncated) {
  EXPECT_STREQ(exception("This is a {} that will not {} and it will be {} because it's too {}. Nowadays the screens "
                         "are wide so we need to increase the max size of the exception.",
                         "string", "fit inside the buffer", "truncated", "long")
                   .what(),
               "This is a string that will not fit inside the buffer and it will be truncated becaus...");
}

}  // namespace aeronet