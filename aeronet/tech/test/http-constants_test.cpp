#include "aeronet/http-constants.hpp"

#include <gtest/gtest.h>

#include "aeronet/http-status-code.hpp"

namespace aeronet::http {

TEST(HttpConstantsTest, DefaultReasonPhrase) {
  EXPECT_EQ(ReasonPhraseFor(StatusCodeSwitchingProtocols), "Switching Protocols");
  EXPECT_EQ(ReasonPhraseFor(StatusCodeOK), "OK");
  EXPECT_EQ(ReasonPhraseFor(StatusCodeMovedPermanently), "Moved Permanently");
  EXPECT_EQ(ReasonPhraseFor(StatusCodeNotFound), "Not Found");
  EXPECT_EQ(ReasonPhraseFor(StatusCodeMethodNotAllowed), "Method Not Allowed");
  EXPECT_EQ(ReasonPhraseFor(StatusCodeBadRequest), "Bad Request");
  EXPECT_EQ(ReasonPhraseFor(StatusCodeNotAcceptable), "Not Acceptable");
  EXPECT_EQ(ReasonPhraseFor(StatusCodeURITooLong), "URI Too Long");
  EXPECT_EQ(ReasonPhraseFor(StatusCodeRequestHeaderFieldsTooLarge), "Request Header Fields Too Large");
  EXPECT_EQ(ReasonPhraseFor(StatusCodeInternalServerError), "Internal Server Error");
  EXPECT_EQ(ReasonPhraseFor(static_cast<StatusCode>(599)), "");
}

TEST(HttpConstantsTest, StatusCodeAllowsBody) {
  for (StatusCode code : {
           StatusCodeContinue,
           StatusCodeSwitchingProtocols,
           StatusCodeEarlyHints,
           StatusCode{199},
           StatusCodeNoContent,
           StatusCodeNotModified,
           MagicForHttpRequest,
       }) {
    EXPECT_FALSE(StatusCodeAllowsBody(code)) << code;
  }
  for (StatusCode code : {
           StatusCodeOK,
           StatusCodeResetContent,
           StatusCodePartialContent,
           StatusCodeMovedPermanently,
           StatusCodeSeeOther,
           StatusCodeNotFound,
           StatusCodeInternalServerError,
           StatusCode{599},
       }) {
    EXPECT_TRUE(StatusCodeAllowsBody(code)) << code;
  }
}

}  // namespace aeronet::http