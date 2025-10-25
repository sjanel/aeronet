#include <aeronet/aeronet.hpp>
#include <aeronet/static-file.hpp>
#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <utility>

namespace {
std::atomic_bool gStop{false};
void handleSigint([[maybe_unused]] int signum) { gStop.store(true); }
}  // namespace

int main(int argc, char** argv) {
  uint16_t port = 0;
  std::filesystem::path root = ".";
  if (argc > 1) {
    port = static_cast<uint16_t>(std::stoi(argv[1]));
  }
  if (argc > 2) {
    root = argv[2];
  }

  aeronet::HttpServerConfig cfg;
  cfg.withPort(port);

  aeronet::HttpServer server(std::move(cfg));

  // Serve files rooted at `root` using the StaticFileHandler
  aeronet::StaticFileHandler handler(root);
  server.router().setDefault([handler](const aeronet::HttpRequest& req) mutable { return handler(req); });

  std::signal(SIGINT, handleSigint);
  std::cout << "Starting static file example on port: " << server.port() << " serving root: " << root << '\n';
  server.runUntil([]() { return gStop.load(); });
  return 0;
}
