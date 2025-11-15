#include <aeronet/aeronet.hpp>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <thread>
#include <utility>

#include "aeronet/log.hpp"

using namespace aeronet;

namespace {
volatile std::sig_atomic_t gStop = 0;
void handleSigint([[maybe_unused]] int signum) { gStop = 1; }
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

  try {
    HttpServerConfig cfg;
    cfg.withPort(port).withReusePort(true);
    Router router;
    router.setDefault([](const HttpRequest& req) {
      return HttpResponse(200, "OK").body(std::string("multi reactor response ") + std::string(req.path()) + '\n');
    });

    MultiHttpServer multi(cfg, std::move(router), static_cast<uint32_t>(threads));

    multi.start();
    log::info("Listening on {} with {} reactors (SO_REUSEPORT). Press Ctrl+C to stop.", multi.port(), threads);
    std::signal(SIGINT, handleSigint);
    while (gStop == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    auto stats = multi.stats();
    log::info("Shutting down. reactors={} totalQueued={}", static_cast<size_t>(stats.per.size()),
              static_cast<unsigned long long>(stats.total.totalBytesQueued));
  } catch (const std::exception& ex) {
    std::cerr << "Error: " << ex.what() << '\n';
    return EXIT_FAILURE;
  }
  return 0;
}
