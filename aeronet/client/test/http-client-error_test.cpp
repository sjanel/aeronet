// Unit coverage for HttpClientErrc + ErrcToStr (every enum value maps to a non-empty, distinct message).
#include "aeronet/http-client-error.hpp"

#include <gtest/gtest.h>

#include <array>
#include <string_view>

namespace aeronet {

TEST(HttpClientErrcTest, EveryCodeHasNonEmptyDescription) {
  constexpr std::array kAll{
      HttpClientErrc::invalidUrl,        HttpClientErrc::connectFailed,       HttpClientErrc::tlsError,
      HttpClientErrc::timeout,           HttpClientErrc::writeError,          HttpClientErrc::connectionClosed,
      HttpClientErrc::malformedResponse, HttpClientErrc::protocolUnsupported, HttpClientErrc::ioError,
  };
  for (const HttpClientErrc errc : kAll) {
    EXPECT_FALSE(ErrcToStr(errc).empty()) << static_cast<int>(errc);
  }
}

TEST(HttpClientErrcTest, DescriptionsAreStable) {
  EXPECT_EQ(ErrcToStr(HttpClientErrc::invalidUrl), "malformed or unsupported URL");
  EXPECT_EQ(ErrcToStr(HttpClientErrc::connectFailed), "connection failed");
  EXPECT_EQ(ErrcToStr(HttpClientErrc::tlsError), "TLS handshake failed");
  EXPECT_EQ(ErrcToStr(HttpClientErrc::timeout), "operation timed out");
  EXPECT_EQ(ErrcToStr(HttpClientErrc::writeError), "transport write failed");
  EXPECT_EQ(ErrcToStr(HttpClientErrc::connectionClosed), "connection closed before a complete response was received");
  EXPECT_EQ(ErrcToStr(HttpClientErrc::malformedResponse), "malformed or oversized response");
  EXPECT_EQ(ErrcToStr(HttpClientErrc::protocolUnsupported), "negotiated application protocol is not supported");
  EXPECT_EQ(ErrcToStr(HttpClientErrc::ioError), "internal I/O error");
}

// The "unknown" fallback guards against an out-of-range value (e.g. from a future enum extension or a
// corrupted cast) without throwing.
TEST(HttpClientErrcTest, UnknownCodeFallsBack) {
  EXPECT_EQ(ErrcToStr(static_cast<HttpClientErrc>(0xFF)), "unknown error");
}

}  // namespace aeronet
