#include "aeronet/http-error-build.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>

#include "aeronet/concatenated-headers.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-status-code.hpp"

using namespace aeronet;

TEST(HttpErrorBuildTest, BuildSimpleErrorOnly) {
  // Test a few representative error codes
  for (auto code :
       {static_cast<http::StatusCode>(400), static_cast<http::StatusCode>(404), static_cast<http::StatusCode>(500)}) {
    auto data = BuildSimpleError(code, {}, "Err");
    std::string_view full(data);
    std::string expected = std::string("HTTP/1.1 ") + std::to_string(code);
    EXPECT_TRUE(full.rfind(expected, 0) == 0) << "Response did not start with '" << expected << "':\n" << full;

    // Check required headers exist
    EXPECT_TRUE(full.contains("Content-Length: 0\r\n")) << full;
    EXPECT_TRUE(full.contains("Connection: close\r\n")) << full;

    // Date header should be present and of RFC7231 length (29 chars after 'Date: ')
    auto datePos = full.find("Date: ");
    EXPECT_NE(datePos, std::string_view::npos) << full;
    auto dateEnd = full.find(http::CRLF, datePos);
    EXPECT_NE(dateEnd, std::string_view::npos) << full;
    auto dateLen = dateEnd - (datePos + 6);  // 6 == len("Date: ")
    EXPECT_EQ(dateLen, static_cast<std::size_t>(29))
        << "Date header length unexpected: " << full.substr(datePos, dateEnd - datePos);
  }
}

TEST(HttpErrorBuildTest, BuildSimpleErrorWithGlobalHeaders) {
  // Build a couple of global headers and ensure they are present in the generated error response.
  ConcatenatedHeaders gh;
  gh.append("X-Test: foo");
  gh.append("X-Server: aeronet");

  auto data = BuildSimpleError(http::StatusCode{500}, gh, "Internal");
  std::string_view full(data);

  EXPECT_TRUE(full.contains("X-Test: foo\r\n")) << full;
  EXPECT_TRUE(full.contains("X-Server: aeronet\r\n")) << full;
}

TEST(HttpErrorBuildTest, BuildSimpleErrorUsesDefaultReasonWhenEmpty) {
  // When reason is empty, BuildSimpleError should use http::reasonPhraseFor(status)
  for (auto code : {static_cast<http::StatusCode>(400), static_cast<http::StatusCode>(503)}) {
    auto data = BuildSimpleError(code, {}, "");
    std::string_view full(data);
    std::string expected =
        std::string("HTTP/1.1 ") + std::to_string(code) + " " + std::string(http::reasonPhraseFor(code));
    EXPECT_TRUE(full.rfind(expected, 0) == 0) << "Response did not start with '" << expected << "':\n" << full;
  }
}
