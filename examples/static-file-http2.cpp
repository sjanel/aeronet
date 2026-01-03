/// @file static-file-http2.cpp
/// @brief Serve static files with HTTP/2 support (h2c or TLS ALPN h2).
///
/// Build with HTTP/2 support:
///   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DAERONET_ENABLE_HTTP2=ON
///   cmake --build build -j
///
/// Run (cleartext h2c):
///   ./build/examples/aeronet-static-file-http2 [port] [root]
/// Run (TLS + ALPN h2):
///   ./build/examples/aeronet-static-file-http2 [cert.pem] [key.pem] [port] --tls [root]
///
/// Test examples:
///   curl --http2-prior-knowledge http://localhost:8080/           # h2c prior knowledge
///   curl --http2 http://localhost:8080/                         # h2c upgrade
///   curl -k --http2 https://localhost:8443/                     # ALPN h2 (TLS)

#include <aeronet/aeronet.hpp>
#include <charconv>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

using namespace aeronet;

int main(int argc, char** argv) {
  uint16_t port = 0;
  std::filesystem::path root = std::filesystem::current_path();
  bool useTls = false;
  std::string certPath;
  std::string keyPath;

  // Simple argument parsing to accept either:
  //  - [port] [root]
  //  - [cert] [key] [port] --tls [root]
  if (argc > 1) {
    std::string first = argv[1];
    bool firstLooksLikeFile = (first.find('.') != std::string::npos || first.find('/') != std::string::npos);
    if (firstLooksLikeFile && argc >= 3) {
      certPath = argv[1];
      keyPath = argv[2];
      useTls = true;
      if (argc > 3) {
        const auto [ptr, ec] = std::from_chars(argv[3], argv[3] + std::strlen(argv[3]), port);
        if (ec != std::errc{} || ptr != argv[3] + std::strlen(argv[3])) {
          std::cerr << "Invalid port: " << argv[3] << '\n';
          return EXIT_FAILURE;
        }
      }
      if (argc > 4) {
        if (std::string_view(argv[4]) == "--tls" && argc > 5) {
          root = argv[5];
        } else {
          root = argv[4];
        }
      }
    } else {
      const auto [ptr, ec] = std::from_chars(argv[1], argv[1] + std::strlen(argv[1]), port);
      if (ec != std::errc{} || ptr != argv[1] + std::strlen(argv[1])) {
        std::cerr << "Invalid port: " << argv[1] << '\n';
        return EXIT_FAILURE;
      }
      if (argc > 2) {
        root = argv[2];
      }
    }
  }

  SignalHandler::Enable();

  try {
    // Configure HTTP/2 settings
    Http2Config http2cfg;
    http2cfg.enable = true;
    http2cfg.enableH2c = true;
    http2cfg.enableH2cUpgrade = true;
    http2cfg.maxConcurrentStreams = 100;

    HttpServerConfig cfg;
    cfg.withPort(port);
    cfg.withHttp2(http2cfg);

    if (useTls) {
      if (certPath.empty()) {
        certPath = "server.crt";
      }
      if (keyPath.empty()) {
        keyPath = "server.key";
      }
      cfg.withTlsCertKey(certPath, keyPath).withTlsAlpnProtocols({"h2", "http/1.1"});
    }

    StaticFileConfig sfCfg;
    sfCfg.enableDirectoryIndex = true;

    Router router;
    router.setDefault(StaticFileHandler(root, std::move(sfCfg)));

    SingleHttpServer server(std::move(cfg), std::move(router));

    std::cout << "Static file server listening on port " << server.port();
    if (useTls) {
      std::cout << " (TLS + ALPN h2)";
    } else {
      std::cout << " (cleartext h2c enabled)";
    }
    std::cout << " serving root: " << root << '\n';

    server.run();
  } catch (const std::exception& ex) {
    std::cerr << "Error: " << ex.what() << '\n';
    return EXIT_FAILURE;
  }

  return 0;
}
