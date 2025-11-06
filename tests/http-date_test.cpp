#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <regex>
#include <string>
#include <string_view>
#include <thread>

#include "aeronet/http-constants.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_util.hpp"

using namespace std::chrono_literals;
using namespace aeronet;

namespace {
std::string rawGet(uint16_t port) {
  test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/";
  opt.connection = "close";
  auto resp = test::request(port, opt);
  return resp.value_or("");
}

std::string headerValue(std::string_view resp, std::string_view name) {
  std::string needle = std::string(name) + ": ";
  std::size_t pos = resp.find(needle);
  if (pos == std::string_view::npos) {
    return {};
  }
  std::size_t end = resp.find(http::CRLF, pos);
  if (end == std::string_view::npos) {
    return {};
  }
  return std::string(resp.substr(pos + needle.size(), end - (pos + needle.size())));
}

test::TestServer ts(HttpServerConfig{});
}  // namespace

TEST(HttpDate, PresentAndFormat) {
  auto port = ts.port();
  ts.server.router().setDefault([](const HttpRequest&) { return HttpResponse(http::StatusCodeOK); });
  auto resp = rawGet(port);
  ASSERT_FALSE(resp.empty());
  auto date = headerValue(resp, "Date");
  ASSERT_EQ(29U, date.size()) << date;
  std::regex re("[A-Z][a-z]{2}, [0-9]{2} [A-Z][a-z]{2} [0-9]{4} [0-9]{2}:[0-9]{2}:[0-9]{2} GMT");
  ASSERT_TRUE(std::regex_match(date, re)) << date;
}

TEST(HttpDate, StableWithinSameSecond) {
  auto port = ts.port();
  ts.server.router().setDefault([](const HttpRequest&) { return HttpResponse(http::StatusCodeOK); });

  // To avoid flakiness near a second rollover on slower / contended CI hosts:
  // Probe until the current second is "stable" for at least ~20ms before sampling sequence.
  // (Single RFC7231 date string has fixed length; we extract the HH:MM:SS portion.)
  auto extractHMS = [](std::string_view dateHeader) -> std::string {
    if (dateHeader.size() < 29) {
      return {};
    }
    return std::string(
        dateHeader.substr(17, 8));  // positions of HH:MM:SS in RFC7231 format: "Wdy, DD Mon YYYY HH:MM:SS GMT"
  };

  std::string anchorDate;
  std::string anchorHMS;
  for (int i = 0; i < 50; ++i) {  // up to ~500ms budget
    anchorDate = headerValue(rawGet(port), "Date");
    anchorHMS = extractHMS(anchorDate);
    if (!anchorHMS.empty()) {
      // Sleep a short time and confirm we are still in same second; if not, loop and pick new anchor.
      std::this_thread::sleep_for(20ms);
      auto confirm = headerValue(rawGet(port), "Date");
      if (extractHMS(confirm) == anchorHMS) {
        anchorDate = confirm;  // use the confirmed value
        break;
      }
    }
  }
  ASSERT_FALSE(anchorDate.empty());

  // Take two additional samples and ensure at least two out of the three share the same second.
  // (If we landed exactly on a boundary the anchor may differ, but then the other two should match.)
  auto s2 = headerValue(rawGet(port), "Date");
  auto s3 = headerValue(rawGet(port), "Date");
  std::string h1 = extractHMS(anchorDate);
  std::string h2 = extractHMS(s2);
  std::string h3 = extractHMS(s3);

  int pairs = 0;
  pairs += (h1 == h2) ? 1 : 0;
  pairs += (h1 == h3) ? 1 : 0;
  pairs += (h2 == h3) ? 1 : 0;
  // IMPORTANT: Stop server before the assertion so a failure does not leave the thread running.
  // (ASSERT_* aborts the test function; previously this caused a 300s timeout in CI because the
  // predicate-controlled loop never observed stop=true.)
  ASSERT_GE(pairs, 1) << "Too much drift across second boundaries: '" << anchorDate << "' '" << s2 << "' '" << s3
                      << "'";
}

TEST(HttpDate, ChangesAcrossSecondBoundary) {
  auto port = ts.port();
  ts.server.router().setDefault([](const HttpRequest&) { return HttpResponse(http::StatusCodeOK); });

  auto first = rawGet(port);
  auto d1 = headerValue(first, "Date");
  ASSERT_EQ(29U, d1.size());
  // spin until date changes (max ~1500ms)
  std::string d2;
  for (int i = 0; i < 150; ++i) {
    std::this_thread::sleep_for(10ms);
    d2 = headerValue(rawGet(port), "Date");
    if (d2 != d1 && !d2.empty()) {
      break;
    }
  }
  ASSERT_NE(d1, d2) << "Date header did not change across boundary after waiting";
}
