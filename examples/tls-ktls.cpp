#include <aeronet/aeronet.hpp>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <utility>

int main(int argc, char** argv) {
  if (argc < 3 || argc > 4) {
    std::cerr << "Usage: " << argv[0] << " <cert.pem> <key.pem> [port]\n";
    return EXIT_FAILURE;
  }

  std::string certPath = argv[1];
  std::string keyPath = argv[2];
  uint16_t port = 0;
  if (argc == 4) {
    port = static_cast<uint16_t>(std::stoi(argv[3]));
  }

  aeronet::SignalHandler::Enable();

  try {
    aeronet::HttpServerConfig cfg;
    cfg.withPort(port).withTlsCertKey(certPath, keyPath).withTlsKtlsMode(aeronet::TLSConfig::KtlsMode::Enabled);

    aeronet::Router router;
    router.setDefault([](const aeronet::HttpRequest& req) {
      aeronet::HttpResponse resp(aeronet::http::StatusCodeOK);
      resp.appendBody("Hello from aeronet with kernel TLS!\n");
      resp.appendBody("Path: ");
      resp.appendBody(req.path());
      resp.appendBody("\n");
      return resp;
    });

    aeronet::SingleHttpServer server(std::move(cfg), std::move(router));

    server.run();

    const auto stats = server.stats();
    std::cout << "kTLS send enabled connections: " << stats.ktlsSendEnabledConnections << '\n';
    std::cout << "kTLS send fallbacks: " << stats.ktlsSendEnableFallbacks << '\n';
    std::cout << "kTLS forced shutdowns: " << stats.ktlsSendForcedShutdowns << '\n';
    std::cout << "kTLS bytes sent via kernel TLS: " << stats.ktlsSendBytes << '\n';
  } catch (const std::exception& ex) {
    std::cerr << "Error: " << ex.what() << '\n';
    return EXIT_FAILURE;
  }
}
