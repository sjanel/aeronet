#include <aeronet/aeronet.hpp>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <utility>

int main(int argc, char** argv) {
  uint16_t port = 0;
  std::filesystem::path root = ".";
  if (argc > 1) {
    port = static_cast<uint16_t>(std::stoi(argv[1]));
  }
  if (argc > 2) {
    root = argv[2];
  }

  aeronet::SignalHandler::Enable();

  try {
    aeronet::HttpServerConfig cfg;
    cfg.withPort(port);

    // Serve files rooted at `root` using the StaticFileHandler
    aeronet::StaticFileConfig staticCfg;
    staticCfg.enableDirectoryIndex = true;  // render HTML listings when index.html is missing

    aeronet::Router router;
    router.setDefault(aeronet::StaticFileHandler(root, std::move(staticCfg)));

    aeronet::SingleHttpServer server(std::move(cfg), std::move(router));

    std::cout << "Starting static file example on port: " << server.port() << " serving root: " << root << '\n';
    server.run();
  } catch (const std::exception& ex) {
    std::cerr << "Error: " << ex.what() << '\n';
    return EXIT_FAILURE;
  }

  return 0;
}
