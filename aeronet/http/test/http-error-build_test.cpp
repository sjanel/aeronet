#include "http-error-build.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include "aeronet/http-constants.hpp"
#include "aeronet/http-header.hpp"
#include "aeronet/http-status-code.hpp"

using namespace aeronet;

TEST(HttpErrorBuildTest, BuildSimpleErrorOnly) {
  std::vector<http::Header> globalHeaders;
  // Test a few representative error codes
  for (auto code :
       {static_cast<http::StatusCode>(400), static_cast<http::StatusCode>(404), static_cast<http::StatusCode>(500)}) {
    auto data = BuildSimpleError(code, std::span<const http::Header>(globalHeaders), "Err");
    std::string full(data.firstBuffer());
    full.append(data.secondBuffer());
    std::string expected = std::string("HTTP/1.1 ") + std::to_string(code);
    EXPECT_TRUE(full.rfind(expected, 0) == 0) << "Response did not start with '" << expected << "':\n" << full;

    // Check required headers exist
    EXPECT_TRUE(full.contains(std::string("Content-Length: 0\r\n"))) << full;
    EXPECT_TRUE(full.contains(std::string("Connection: close\r\n"))) << full;

    // Date header should be present and of RFC7231 length (29 chars after 'Date: ')
    auto datePos = full.find("Date: ");
    EXPECT_NE(datePos, std::string::npos) << full;
    auto dateEnd = full.find(http::CRLF, datePos);
    EXPECT_NE(dateEnd, std::string::npos) << full;
    auto dateLen = dateEnd - (datePos + 6);  // 6 == len("Date: ")
    EXPECT_EQ(dateLen, static_cast<std::size_t>(29))
        << "Date header length unexpected: " << full.substr(datePos, dateEnd - datePos);
  }
}
