#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aeronet/http-response.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/reserved-headers.hpp"

namespace aeronet {
namespace {

// Deterministic PRNG for reproducibility
class FuzzRng {
 public:
  explicit FuzzRng(uint64_t seed) : _gen(seed) {}

  uint8_t byte() { return static_cast<uint8_t>(_dist(_gen)); }

  uint32_t u32() { return static_cast<uint32_t>(_dist(_gen)) | (static_cast<uint32_t>(_dist(_gen)) << 8); }

  std::size_t range(std::size_t lo, std::size_t hi) {
    if (lo >= hi) {
      return lo;
    }
    return lo + (u32() % (hi - lo));
  }

  bool coin() { return (byte() & 1) != 0; }

  std::string randomString(std::size_t maxLen) {
    std::size_t len = range(0, maxLen);
    std::string result;
    result.reserve(len);
    for (std::size_t ii = 0; ii < len; ++ii) {
      result.push_back(static_cast<char>(byte()));
    }
    return result;
  }

  std::string randomPrintableString(std::size_t maxLen) {
    std::size_t len = range(0, maxLen);
    std::string result;
    result.reserve(len);
    for (std::size_t ii = 0; ii < len; ++ii) {
      result.push_back(static_cast<char>(range(32, 127)));
    }
    return result;
  }

  // Generate a valid HTTP token (tchar characters only, non-empty)
  std::string randomToken(std::size_t minLen, std::size_t maxLen) {
    // Characters valid for tchar
    static constexpr std::string_view kTchars =
        "!#$%&'*+-.0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ^_`abcdefghijklmnopqrstuvwxyz|~";
    std::size_t len = std::max(minLen, range(minLen, maxLen + 1));
    std::string result;
    result.reserve(len);
    for (std::size_t ii = 0; ii < len; ++ii) {
      result.push_back(kTchars[range(0, kTchars.size())]);
    }
    return result;
  }

 private:
  std::mt19937_64 _gen;
  std::uniform_int_distribution<uint16_t> _dist{0, 255};
};

// Exercise HttpResponse construction and mutation with random data
void FuzzHttpResponseOperations(FuzzRng& rng) {
  // Random status code (some valid, some not)
  http::StatusCode statusCode;
  if (rng.coin()) {
    static constexpr std::array kValidCodes = {
        http::StatusCodeOK,
        http::StatusCodeCreated,
        http::StatusCodeNoContent,
        http::StatusCodeMovedPermanently,
        http::StatusCodeFound,
        http::StatusCodeBadRequest,
        http::StatusCodeUnauthorized,
        http::StatusCodeForbidden,
        http::StatusCodeNotFound,
        http::StatusCodeMethodNotAllowed,
        http::StatusCodeInternalServerError,
        http::StatusCodeBadGateway,
        http::StatusCodeServiceUnavailable,
    };
    statusCode = kValidCodes[rng.range(0, kValidCodes.size())];
  } else {
    statusCode = static_cast<http::StatusCode>(rng.range(100, 600));
  }

  // Random reason phrase
  std::string reason = rng.coin() ? rng.randomPrintableString(50) : "";

  // Construct with or without initial capacity
  HttpResponse resp =
      rng.coin() ? HttpResponse(statusCode, reason) : HttpResponse(rng.range(64, 2048), statusCode, reason);

  // Random operations
  std::size_t numOps = rng.range(0, 20);
  for (std::size_t op = 0; op < numOps; ++op) {
    std::size_t opType = rng.range(0, 10);

    switch (opType) {
      case 0: {  // Set status
        auto newStatus = static_cast<http::StatusCode>(rng.range(100, 600));
        resp.status(newStatus);
        break;
      }
      case 1: {  // Set reason
        std::string newReason = rng.randomPrintableString(100);
        resp.reason(newReason);
        break;
      }
      case 2: {  // Set body (string_view)
        std::string bodyContent = rng.randomString(rng.range(0, 1000));
        resp.body(std::string_view(bodyContent));
        break;
      }
      case 3: {  // Set body (string move)
        std::string bodyContent = rng.randomString(rng.range(0, 1000));
        resp.body(std::move(bodyContent));
        break;
      }
      case 4: {  // Set body (vector<char>)
        std::size_t len = rng.range(0, 500);
        std::vector<char> bodyVec(len);
        for (std::size_t ii = 0; ii < len; ++ii) {
          bodyVec[ii] = static_cast<char>(rng.byte());
        }
        resp.body(std::move(bodyVec));
        break;
      }
      case 5: {  // Add header
        std::string key = rng.randomToken(1, 30);
        std::string value = rng.randomPrintableString(100);
        if (!http::IsReservedResponseHeader(key)) {
          resp.addHeader(key, value);
        }
        break;
      }
      case 6: {  // Set/replace header
        std::string key = rng.randomToken(1, 30);
        std::string value = rng.randomPrintableString(100);
        if (!http::IsReservedResponseHeader(key)) {
          resp.header(key, value);
        }
        break;
      }
      case 7: {  // Append header value
        std::string key = rng.randomToken(1, 30);
        std::string value = rng.randomPrintableString(50);
        if (!http::IsReservedResponseHeader(key)) {
          resp.appendHeaderValue(key, value);
        }
        break;
      }
      case 8: {  // Read back values (should not crash)
        [[maybe_unused]] auto st = resp.status();
        [[maybe_unused]] auto rs = resp.reason();
        [[maybe_unused]] auto bd = resp.body();
        break;
      }
      case 9: {  // Query header value
        std::string key = rng.randomToken(1, 30);
        [[maybe_unused]] auto val = resp.headerValueOrEmpty(key);
        [[maybe_unused]] auto optVal = resp.headerValue(key);
        break;
      }
      default:
        break;
    }
  }

  // Final reads to ensure state is consistent
  [[maybe_unused]] auto finalStatus = resp.status();
  [[maybe_unused]] auto finalReason = resp.reason();
  [[maybe_unused]] auto finalBody = resp.body();
}

}  // namespace

// Fuzz test HttpResponse with random operations
TEST(HttpResponseFuzzTest, RandomOperations) {
  constexpr std::size_t kIterations = 10000;

  for (std::size_t seed = 0; seed < kIterations; ++seed) {
    FuzzRng rng(seed);
    ASSERT_NO_FATAL_FAILURE(FuzzHttpResponseOperations(rng));
  }
}

// Fuzz test with known header names
TEST(HttpResponseFuzzTest, KnownHeaderStress) {
  constexpr std::size_t kIterations = 5000;

  static constexpr std::array kKnownHeaders = {
      "Content-Type",
      "Cache-Control",
      "X-Custom-Header",
      "Accept",
      "Accept-Encoding",
      "Accept-Language",
      "Access-Control-Allow-Origin",
      "Access-Control-Allow-Methods",
      "Access-Control-Allow-Headers",
      "X-Frame-Options",
      "X-Content-Type-Options",
      "Strict-Transport-Security",
      "Location",
      "ETag",
      "Last-Modified",
      "Expires",
      "Pragma",
      "Vary",
      "Server",
      "X-Powered-By",
  };

  for (std::size_t seed = 0; seed < kIterations; ++seed) {
    FuzzRng rng(seed + 4000000);

    HttpResponse resp(http::StatusCodeOK);

    // Add many headers with known names
    std::size_t numHeaders = rng.range(1, 50);
    for (std::size_t hh = 0; hh < numHeaders; ++hh) {
      std::string_view key = kKnownHeaders[rng.range(0, kKnownHeaders.size())];
      std::string value = rng.randomPrintableString(100);

      if (rng.coin()) {
        resp.addHeader(key, value);
      } else {
        resp.header(key, value);
      }
    }

    // Set body
    std::string body = rng.randomString(rng.range(0, 500));
    resp.body(std::move(body));

    // Read back
    for (const auto& hdr : kKnownHeaders) {
      [[maybe_unused]] auto val = resp.headerValueOrEmpty(hdr);
    }

    SUCCEED();
  }
}

// Fuzz test with trailer operations
TEST(HttpResponseFuzzTest, TrailerStress) {
  constexpr std::size_t kIterations = 3000;

  for (std::size_t seed = 0; seed < kIterations; ++seed) {
    FuzzRng rng(seed + 5000000);

    HttpResponse resp(http::StatusCodeOK);

    // Must set non-empty body before trailers
    std::size_t bodyLen = rng.range(10, 500);
    std::string body;
    body.reserve(bodyLen);
    for (std::size_t ii = 0; ii < bodyLen; ++ii) {
      body.push_back(static_cast<char>(rng.byte()));
    }
    resp.body(std::move(body));

    // Add trailers (use valid token names)
    std::size_t numTrailers = rng.range(0, 10);
    for (std::size_t tt = 0; tt < numTrailers; ++tt) {
      std::string name = rng.randomToken(1, 20);
      std::string value = rng.randomPrintableString(50);
      resp.addTrailer(name, value);
    }

    // Read trailers
    [[maybe_unused]] auto trailers = resp.trailers();

    SUCCEED();
  }
}

// Fuzz test with edge case status codes and reasons
TEST(HttpResponseFuzzTest, StatusCodeEdgeCases) {
  // Test boundary status codes
  for (int code = 100; code < 600; ++code) {
    HttpResponse resp(static_cast<http::StatusCode>(code));
    EXPECT_EQ(static_cast<int>(resp.status()), code);
  }

  // Test with various reason phrases
  static const std::array<std::string_view, 10> kReasons = {
      "",
      "OK",
      "Not Found",
      "Internal Server Error",
      std::string_view("Reason with \x00 null", 18),  // Contains null
      "Very long reason phrase that goes on and on and on and on and on and on and on",
      "Special chars: !@#$%^&*()[]{}|\\;':\",./<>?",
      "Unicode: café résumé naïve",
      "Tabs\tand\tnewlines\nhere",
      "   Leading and trailing spaces   ",
  };

  for (const auto& reason : kReasons) {
    HttpResponse resp(http::StatusCodeOK, reason);
    // Just ensure no crash
    [[maybe_unused]] auto rs = resp.reason();
    SUCCEED();
  }
}

// Fuzz test body with various content types
TEST(HttpResponseFuzzTest, BodyContentTypes) {
  constexpr std::size_t kIterations = 2000;

  static constexpr std::array kContentTypes = {
      "text/plain",
      "text/html",
      "text/html; charset=utf-8",
      "application/json",
      "application/xml",
      "application/octet-stream",
      "image/png",
      "image/jpeg",
      "application/pdf",
      "multipart/form-data; boundary=----WebKitFormBoundary",
  };

  for (std::size_t seed = 0; seed < kIterations; ++seed) {
    FuzzRng rng(seed + 6000000);

    HttpResponse resp(http::StatusCodeOK);

    std::string_view contentType = kContentTypes[rng.range(0, kContentTypes.size())];
    std::string body = rng.randomString(rng.range(0, 1000));

    resp.body(std::string_view(body), contentType);

    [[maybe_unused]] auto bd = resp.body();
    [[maybe_unused]] auto ct = resp.headerValueOrEmpty("Content-Type");

    SUCCEED();
  }
}

// Stress test with many sequential operations
TEST(HttpResponseFuzzTest, SequentialMutationStress) {
  constexpr std::size_t kIterations = 1000;

  for (std::size_t seed = 0; seed < kIterations; ++seed) {
    FuzzRng rng(seed + 7000000);

    HttpResponse resp(http::StatusCodeOK);

    // Many status changes
    for (std::size_t ii = 0; ii < 50; ++ii) {
      resp.status(static_cast<http::StatusCode>(rng.range(100, 600)));
    }

    // Many reason changes
    for (std::size_t ii = 0; ii < 50; ++ii) {
      resp.reason(rng.randomPrintableString(30));
    }

    // Many body changes
    for (std::size_t ii = 0; ii < 20; ++ii) {
      resp.body(rng.randomString(rng.range(0, 200)));
    }

    // Many header additions
    for (std::size_t ii = 0; ii < 100; ++ii) {
      std::string key = "X-Header-" + std::to_string(ii);
      resp.addHeader(key, rng.randomPrintableString(50));
    }

    // Final state check
    [[maybe_unused]] auto st = resp.status();
    [[maybe_unused]] auto rs = resp.reason();
    [[maybe_unused]] auto bd = resp.body();

    SUCCEED();
  }
}

}  // namespace aeronet
