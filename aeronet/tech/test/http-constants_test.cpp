#include "aeronet/http-constants.hpp"

#include <gtest/gtest.h>

#include "aeronet/http-status-code.hpp"

namespace aeronet::http {

TEST(HttpConstantsTest, DefaultReasonPhrase) {
  EXPECT_EQ(ReasonPhraseFor(StatusCodeOK), "OK");
  EXPECT_EQ(ReasonPhraseFor(StatusCodeMovedPermanently), "Moved Permanently");
  EXPECT_EQ(ReasonPhraseFor(StatusCodeNotFound), "Not Found");
  EXPECT_EQ(ReasonPhraseFor(StatusCodeMethodNotAllowed), "Method Not Allowed");
  EXPECT_EQ(ReasonPhraseFor(StatusCodeBadRequest), "Bad Request");
  EXPECT_EQ(ReasonPhraseFor(StatusCodeNotAcceptable), "Not Acceptable");
  EXPECT_EQ(ReasonPhraseFor(StatusCodeInternalServerError), "Internal Server Error");
  EXPECT_EQ(ReasonPhraseFor(static_cast<StatusCode>(599)), "");
}

}  // namespace aeronet::http