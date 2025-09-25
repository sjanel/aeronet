// Multi-reactor example using MultiHttpServer convenience wrapper.
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/multi-http-server.hpp"
#include "log.hpp"

namespace {
volatile std::sig_atomic_t gStop = 0;
void handleSigint(int /*signum*/) { gStop = 1; }
}  // namespace

int main(int argc, char** argv) {
  uint16_t port = 8080;
  int threads = 4;  // default
  if (argc > 1) {
    port = static_cast<uint16_t>(std::stoi(argv[1]));
  }
  if (argc > 2) {
    threads = std::stoi(argv[2]);
  }

  aeronet::HttpServerConfig cfg;
  cfg.withPort(port).withReusePort(true);
  aeronet::MultiHttpServer multi(cfg, static_cast<uint32_t>(threads));
  multi.setHandler([](const aeronet::HttpRequest& req) {
    aeronet::HttpResponse resp;
    resp.statusCode = 200;
    resp.reason = "OK";
    resp.contentType = "text/plain";
    resp.body = std::string("multi reactor response ") + std::string(req.target);
    return resp;
  });
  multi.start();
  aeronet::log::info("Listening on {} with {} reactors (SO_REUSEPORT). Press Ctrl+C to stop.", multi.port(), threads);
  std::signal(SIGINT, handleSigint);
  while (gStop == 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  auto stats = multi.stats();
  aeronet::log::info("Shutting down. reactors={} totalQueued={}", static_cast<size_t>(stats.per.size()),
                     static_cast<unsigned long long>(stats.total.totalBytesQueued));
  multi.stop();
  return 0;
}
