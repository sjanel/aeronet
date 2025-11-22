#include <aeronet/aeronet.hpp>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <utility>

#include "aeronet/log.hpp"

using namespace aeronet;

int main(int argc, char** argv) {
  uint16_t port = 8080;
  int threads = 4;  // default
  if (argc > 1) {
    port = static_cast<uint16_t>(std::stoi(argv[1]));
  }
  if (argc > 2) {
    threads = std::stoi(argv[2]);
  }

  aeronet::SignalHandler::Enable();

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

    auto stats = multi.stats();
    log::info("Shutting down. reactors={} totalQueued={}", static_cast<size_t>(stats.per.size()),
              static_cast<unsigned long long>(stats.total.totalBytesQueued));
    multi.run();
  } catch (const std::exception& ex) {
    std::cerr << "Error: " << ex.what() << '\n';
    return EXIT_FAILURE;
  }
  return 0;
}
