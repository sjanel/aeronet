/// @file http2.cpp
/// @brief Minimal HTTP/2 server example demonstrating ALPN (h2) and h2c upgrade.
///
/// Build with HTTP/2 support:
///   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DAERONET_ENABLE_HTTP2=ON
///   cmake --build build -j
///
/// Run:
///   ./build/examples/aeronet-http2                    # Cleartext h2c on ephemeral port
///   ./build/examples/aeronet-http2 8080               # Cleartext h2c on port 8080
///   ./build/examples/aeronet-http2 8443 --tls         # HTTPS with ALPN "h2" (requires certs)
///
/// Test:
///   curl --http2-prior-knowledge http://localhost:8080/hello  # h2c prior knowledge
///   curl --http2 http://localhost:8080/hello                  # h2c upgrade
///   curl -k --http2 https://localhost:8443/hello              # ALPN h2

#include <aeronet/aeronet.hpp>
#include <aeronet/http2-config.hpp>
#include <charconv>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

using namespace aeronet;

int main(int argc, char** argv) {
  uint16_t port = 0;
  bool useTls = false;
  std::string certPath;
  std::string keyPath;

  // Parse arguments: [cert] [key] [port] [--tls]
  // or: [port] [--tls]
  if (argc > 1) {
    // Check if first arg is a cert file (exists and is readable)
    std::string firstArg = argv[1];
    bool firstArgIsFile = (firstArg.find('.') != std::string::npos && firstArg.find('/') != std::string::npos);

    if (firstArgIsFile && argc >= 3) {
      // Cert/key path format: cert key [port] [--tls]
      certPath = argv[1];
      keyPath = argv[2];
      useTls = true;

      if (argc > 3) {
        const auto [ptr, errc] = std::from_chars(argv[3], argv[3] + std::strlen(argv[3]), port);
        if (errc != std::errc{} || ptr != argv[3] + std::strlen(argv[3])) {
          std::cerr << "Invalid port number: " << argv[3] << "\n";
          return EXIT_FAILURE;
        }
      }
    } else {
      // Port-first format: [port] [--tls]
      const auto [ptr, errc] = std::from_chars(argv[1], argv[1] + std::strlen(argv[1]), port);
      if (errc != std::errc{} || ptr != argv[1] + std::strlen(argv[1])) {
        std::cerr << "Invalid port number: " << argv[1] << "\n";
        return EXIT_FAILURE;
      }

      if (argc > 2) {
        useTls = (std::string_view(argv[2]) == "--tls");
      }
    }
  }

  // Enable signal handler for graceful shutdown on Ctrl+C
  SignalHandler::Enable();

  Router router;

  try {
    // Unified handler for both HTTP/1.1 and HTTP/2
    // The handler receives an HttpRequest which has isHttp2() method to detect protocol
    router.setDefault([](const HttpRequest& req) {
      HttpResponse resp(200);
      if (req.isHttp2()) {
        resp.appendBody("Hello from aeronet HTTP/2!\n");
        resp.appendBody("Stream ID: ");
        resp.appendBody(std::to_string(req.streamId()));
        resp.appendBody("\n");
      } else {
        resp.appendBody("Hello from aeronet HTTP/1.1!\n");
      }
      resp.appendBody("Path: ");
      resp.appendBody(req.path());
      resp.appendBody("\nMethod: ");
      resp.appendBody(http::MethodToStr(req.method()));
      resp.appendBody("\n");
      return resp;
    });

    // Configure HTTP/2
    Http2Config http2Config;
    http2Config.enable = true;
    http2Config.maxConcurrentStreams = 100;
    http2Config.initialWindowSize = 65535;
    http2Config.enableH2c = true;         // Allow cleartext HTTP/2 (prior knowledge)
    http2Config.enableH2cUpgrade = true;  // Allow HTTP/1.1 -> HTTP/2 upgrade

    // Configure server with HTTP/2 settings
    HttpServerConfig config;
    config.withPort(port).withHttp2(http2Config);

    // Configure TLS if requested
    if (useTls) {
      // Use provided cert/key paths, or defaults
      if (certPath.empty()) {
        certPath = "server.crt";
      }
      if (keyPath.empty()) {
        keyPath = "server.key";
      }
      config.withTlsCertKey(certPath, keyPath).withTlsAlpnProtocols({"h2", "http/1.1"});  // Prefer HTTP/2 via ALPN
    }

    SingleHttpServer server(std::move(config), std::move(router));

    std::cout << "HTTP/2 server listening on port " << server.port();
    if (useTls) {
      std::cout << " (TLS with ALPN h2)\n";
      std::cout << "Test with: curl -k --http2 https://localhost:" << server.port() << "/hello\n";
    } else {
      std::cout << " (cleartext h2c)\n";
      std::cout << "Test with h2c prior knowledge: curl --http2-prior-knowledge http://localhost:" << server.port()
                << "/hello\n";
      std::cout << "Test with h2c upgrade: curl --http2 http://localhost:" << server.port() << "/hello\n";
    }

    server.run();  // blocking run, until Ctrl+C
  } catch (const std::exception& e) {
    std::cerr << "Server encountered error: " << e.what() << '\n';
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
