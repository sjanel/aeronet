#include <arpa/inet.h>
#include <benchmark/benchmark.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"

// Self-contained minimal roundtrip benchmark (loopback). Avoids depending on test utilities
// so the benchmarks module can stay decoupled from test headers.

namespace {
class MinimalServerFixture : public benchmark::Fixture {
 protected:
  void SetUp(const benchmark::State& state [[maybe_unused]]) override {
    stopFlag.store(false, std::memory_order_relaxed);
    server = std::make_unique<aeronet::HttpServer>(aeronet::HttpServerConfig{}.withPort(0));
    server->setHandler([](const aeronet::HttpRequest&) {
      aeronet::HttpResponse resp;
      resp.body("OK");
      return resp;
    });
    loopThread =
        std::jthread([this] { server->runUntil([this] { return stopFlag.load(std::memory_order_relaxed); }); });
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  void TearDown(const benchmark::State& state [[maybe_unused]]) override {
    stopFlag.store(true, std::memory_order_relaxed);
    if (loopThread.joinable()) {
      loopThread.join();
    }
    server.reset();
  }
  std::unique_ptr<aeronet::HttpServer> server;
  std::atomic<bool> stopFlag{false};
  std::jthread loopThread;
};

bool sendGet(uint16_t port) {
  // TODO: use ClientConnection from test_util.hpp
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return false;
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    ::close(fd);
    return false;
  }
  const char req[] = "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
  ssize_t sent = ::send(fd, req, sizeof(req) - 1, 0);
  if (sent != static_cast<ssize_t>(sizeof(req) - 1)) {
    ::close(fd);
    return false;
  }
  char buf[512];
  // Read until close
  // Read with a short timeout loop instead of blocking indefinitely in case of server issues
  auto start = std::chrono::steady_clock::now();
  using namespace std::chrono_literals;
  while (std::chrono::steady_clock::now() - start < 200ms) {
    ssize_t bytes = ::recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
    if (bytes > 0) {
      // minimal validation: look for end of headers
      if (std::string_view(buf, static_cast<size_t>(bytes)).find("\r\n\r\n") != std::string_view::npos) {
        break;
      }
      continue;
    }
    if (bytes == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      std::this_thread::sleep_for(2ms);
      continue;
    }
    break;  // connection closed or error
  }
  ::close(fd);
  return true;
}

BENCHMARK_F(MinimalServerFixture, GET_RoundTrip)(benchmark::State& state) {
  auto portNumber = server->port();
  for (auto iter : state) {
    (void)iter;
    if (!sendGet(portNumber)) {
      state.SkipWithError("roundtrip failed");
      break;
    }
  }
  state.counters["port"] = static_cast<double>(portNumber);
}
}  // namespace

BENCHMARK_MAIN();
