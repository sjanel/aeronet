#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <regex>
#include <string>
#include <thread>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/server-config.hpp"
#include "aeronet/server.hpp"
#include "test_http_client.hpp"

using namespace std::chrono_literals;

namespace {
std::string rawGet(uint16_t port) {
  test_http_client::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/";
  opt.connection = "close";
  auto resp = test_http_client::request(port, opt);
  return resp.value_or("");
}

std::string headerValue(const std::string& resp, const std::string& name) {
  std::string needle = name + ": ";
  std::size_t pos = resp.find(needle);
  if (pos == std::string::npos) {
    return {};
  }
  std::size_t end = resp.find("\r\n", pos);
  if (end == std::string::npos) {
    return {};
  }
  return resp.substr(pos + needle.size(), end - (pos + needle.size()));
}
}  // namespace

TEST(HttpDate, PresentAndFormat) {
  std::atomic_bool stop{false};
  aeronet::HttpServer server(aeronet::ServerConfig{});
  auto port = server.port();
  server.setHandler([](const aeronet::HttpRequest&) { return aeronet::HttpResponse{}; });
  std::jthread th([&] { server.runUntil([&] { return stop.load(); }, 50ms); });
  std::this_thread::sleep_for(100ms);
  auto resp = rawGet(port);
  stop.store(true);
  th.join();
  ASSERT_FALSE(resp.empty());
  auto date = headerValue(resp, "Date");
  ASSERT_EQ(29U, date.size()) << date;
  std::regex re("[A-Z][a-z]{2}, [0-9]{2} [A-Z][a-z]{2} [0-9]{4} [0-9]{2}:[0-9]{2}:[0-9]{2} GMT");
  ASSERT_TRUE(std::regex_match(date, re)) << date;
}

TEST(HttpDate, StableWithinSameSecond) {
  std::atomic_bool stop{false};
  aeronet::HttpServer server(aeronet::ServerConfig{});
  auto port = server.port();
  server.setHandler([](const aeronet::HttpRequest&) { return aeronet::HttpResponse{}; });
  std::jthread th([&] { server.runUntil([&] { return stop.load(); }, 10ms); });
  std::this_thread::sleep_for(30ms);

  // To avoid flakiness near a second rollover on slower / contended CI hosts:
  // Probe until the current second is "stable" for at least ~20ms before sampling sequence.
  // (Single RFC7231 date string has fixed length; we extract the HH:MM:SS portion.)
  auto extractHMS = [](const std::string& dateHeader) -> std::string {
    if (dateHeader.size() < 29) {
      return {};
    }
    return dateHeader.substr(17, 8);  // positions of HH:MM:SS in RFC7231 format: "Wdy, DD Mon YYYY HH:MM:SS GMT"
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

  // Now perform rapid sequence; allow at most one rollover among the three (rare) but require
  // at least two to match the anchor to stay resilient on very slow environments.
  auto s2 = headerValue(rawGet(port), "Date");
  auto s3 = headerValue(rawGet(port), "Date");
  std::string h1 = extractHMS(anchorDate);
  std::string h2 = extractHMS(s2);
  std::string h3 = extractHMS(s3);

  int sameAsAnchor = static_cast<int>(h1 == h2) + static_cast<int>(h1 == h3);
  // We expect at least two out of the three samples to share the same second if sampling mid-second.
  ASSERT_GE(sameAsAnchor, 1) << "Too much drift across second boundaries: '" << anchorDate << "' '" << s2 << "' '" << s3
                             << "'";
  stop.store(true);
  th.join();
}

TEST(HttpDate, ChangesAcrossSecondBoundary) {
  std::atomic_bool stop{false};
  aeronet::HttpServer server(aeronet::ServerConfig{});
  auto port = server.port();
  server.setHandler([](const aeronet::HttpRequest&) { return aeronet::HttpResponse{}; });
  std::jthread th([&] { server.runUntil([&] { return stop.load(); }, 5ms); });
  std::this_thread::sleep_for(50ms);
  auto first = rawGet(port);
  auto d1 = headerValue(first, "Date");
  ASSERT_EQ(29U, d1.size());
  // spin until date changes (max ~1500ms)
  std::string d2;
  for (int i = 0; i < 40; ++i) {
    std::this_thread::sleep_for(50ms);
    d2 = headerValue(rawGet(port), "Date");
    if (d2 != d1 && !d2.empty()) {
      break;
    }
  }
  stop.store(true);
  th.join();
  ASSERT_NE(d1, d2) << "Date header did not change across boundary after waiting";
}
