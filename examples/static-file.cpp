#include <aeronet/aeronet.hpp>
#include <aeronet/static-file-handler.hpp>
#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
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

  try {
    aeronet::HttpServerConfig cfg;
    cfg.withPort(port);

    aeronet::HttpServer server(std::move(cfg));

    // Serve files rooted at `root` using the StaticFileHandler
    aeronet::StaticFileConfig staticCfg;
    staticCfg.enableDirectoryIndex = true;  // render HTML listings when index.html is missing
    server.router().setDefault(aeronet::StaticFileHandler(root, std::move(staticCfg)));

    std::signal(SIGINT, handleSigint);
    std::cout << "Starting static file example on port: " << server.port() << " serving root: " << root << '\n';
    server.runUntil([]() { return gStop.load(); });
  } catch (const std::exception& ex) {
    std::cerr << "Error during setup: " << ex.what() << '\n';
    return EXIT_FAILURE;
  }

  return 0;
}
