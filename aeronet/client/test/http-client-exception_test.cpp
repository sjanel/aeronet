// Unit coverage for HttpClientException (both constructors + what()).
#include "aeronet/http-client-exception.hpp"

#include <gtest/gtest.h>

#include <string>

namespace aeronet {

TEST(HttpClientExceptionTest, FromStdString) {
  const std::string msg = "boom from std::string";
  HttpClientException ex(msg);
  EXPECT_STREQ(ex.what(), "boom from std::string");
}

TEST(HttpClientExceptionTest, FromCString) {
  HttpClientException ex("boom from c-string");
  EXPECT_STREQ(ex.what(), "boom from c-string");
}

TEST(HttpClientExceptionTest, IsRuntimeError) {
  try {
    throw HttpClientException("x");
  } catch (const std::runtime_error& err) {
    EXPECT_STREQ(err.what(), "x");
  }
}

}  // namespace aeronet
