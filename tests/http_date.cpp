#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <regex>
#include <string>
#include <thread>

#include "aeronet/server.hpp"

using namespace std::chrono_literals;

namespace {
std::string rawGet(uint16_t port) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return {};
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    ::close(fd);
    return {};
  }
  std::string req = "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
  ::send(fd, req.data(), req.size(), 0);
  char buf[4096];
  std::string out;
  while (true) {
    ssize_t rcv = ::recv(fd, buf, sizeof(buf), 0);
    if (rcv <= 0) {
      break;
    }
    out.append(buf, buf + rcv);
  }
  ::close(fd);
  return out;
}

std::string headerValue(const std::string& resp, const std::string& name) {
  std::string needle = name + ": ";
  size_t p = resp.find(needle);
  if (p == std::string::npos) {
    return {};
  }
  size_t end = resp.find("\r\n", p);
  if (end == std::string::npos) {
    return {};
  }
  return resp.substr(p + needle.size(), end - (p + needle.size()));
}
}  // namespace

TEST(HttpDate, PresentAndFormat) {
  std::atomic_bool stop{false};
  uint16_t port = 18090;
  aeronet::HttpServer server(aeronet::ServerConfig{}.withPort(port));
  server.setHandler([](const aeronet::HttpRequest&) { return aeronet::HttpResponse{}; });
  std::thread th([&] { server.runUntil([&] { return stop.load(); }, 50ms); });
  std::this_thread::sleep_for(100ms);
  auto resp = rawGet(port);
  stop.store(true);
  th.join();
  ASSERT_FALSE(resp.empty());
  auto date = headerValue(resp, "Date");
  ASSERT_EQ(29u, date.size()) << date;
  std::regex re("[A-Z][a-z]{2}, [0-9]{2} [A-Z][a-z]{2} [0-9]{4} [0-9]{2}:[0-9]{2}:[0-9]{2} GMT");
  ASSERT_TRUE(std::regex_match(date, re)) << date;
}

TEST(HttpDate, StableWithinSameSecond) {
  std::atomic_bool stop{false};
  uint16_t port = 18091;
  aeronet::HttpServer server(aeronet::ServerConfig{}.withPort(port));
  server.setHandler([](const aeronet::HttpRequest&) { return aeronet::HttpResponse{}; });
  std::thread th([&] { server.runUntil([&] { return stop.load(); }, 10ms); });
  std::this_thread::sleep_for(50ms);
  auto first = rawGet(port);
  auto d1 = headerValue(first, "Date");
  // Issue two more requests quickly; they should have identical Date header if within same second
  auto second = rawGet(port);
  auto d2 = headerValue(second, "Date");
  auto third = rawGet(port);
  auto d3 = headerValue(third, "Date");
  stop.store(true);
  th.join();
  ASSERT_EQ(d1, d2);
  ASSERT_EQ(d1, d3);
}

TEST(HttpDate, ChangesAcrossSecondBoundary) {
  std::atomic_bool stop{false};
  uint16_t port = 18092;
  aeronet::HttpServer server(aeronet::ServerConfig{}.withPort(port));
  server.setHandler([](const aeronet::HttpRequest&) { return aeronet::HttpResponse{}; });
  std::thread th([&] { server.runUntil([&] { return stop.load(); }, 5ms); });
  std::this_thread::sleep_for(50ms);
  auto first = rawGet(port);
  auto d1 = headerValue(first, "Date");
  ASSERT_EQ(29u, d1.size());
  // spin until date changes (max ~1500ms)
  std::string d2;
  for (int i = 0; i < 40; ++i) {
    std::this_thread::sleep_for(50ms);
    d2 = headerValue(rawGet(port), "Date");
    if (d2 != d1 && !d2.empty()) break;
  }
  stop.store(true);
  th.join();
  ASSERT_NE(d1, d2) << "Date header did not change across boundary after waiting";
}
