#include <aeronet/aeronet.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <utility>

using namespace aeronet;

/// Example demonstrating TLS session ticket configuration.
///
/// Session tickets allow TLS session resumption without server-side session
/// caches, enabling faster subsequent handshakes (0-RTT negotiation).
///
/// Test session resumption with OpenSSL s_client:
///   # First connection (full handshake)
///   openssl s_client -connect localhost:8443 -sess_out session.pem
///
///   # Second connection (resumed - look for "Reused, TLSv1.3")
///   openssl s_client -connect localhost:8443 -sess_in session.pem

int main(int argc, char** argv) {
  if (argc < 3 || argc > 5) {
    std::cerr << "Usage: " << argv[0] << " <cert.pem> <key.pem> [port] [--static-key]\n\n";
    std::cerr << "Options:\n";
    std::cerr << "  --static-key  Use a static session ticket key instead of auto-rotation\n";
    return EXIT_FAILURE;
  }

  std::string certPath = argv[1];
  std::string keyPath = argv[2];
  uint16_t port = 8443;
  bool useStaticKey = false;

  for (int ii = 3; ii < argc; ++ii) {
    std::string arg = argv[ii];
    if (arg == "--static-key") {
      useStaticKey = true;
    } else {
      port = static_cast<uint16_t>(std::stoi(arg));
    }
  }

  SignalHandler::Enable();

  try {
    HttpServerConfig cfg;
    cfg.withPort(port).withTlsCertKey(certPath, keyPath);

    if (useStaticKey) {
      // Static key mode: load a fixed key for session ticket encryption.
      // This is useful for distributed deployments where multiple servers
      // need to decrypt tickets issued by each other.
      //
      // In production, load this from a secrets manager or encrypted storage.
      // Key must be exactly 48 bytes: 16B name + 16B AES + 16B HMAC.
      TLSConfig::SessionTicketKey staticKey;

      // For demo purposes only - use RAND_bytes() or secure key storage in production
      for (auto& bt : staticKey) {
        bt = static_cast<std::byte>(std::rand() % 256);  // NOLINT - demo only
      }

      cfg.tls.withTlsSessionTicketKey(std::move(staticKey));  // Enables tickets + loads key
      std::cout << "Session tickets enabled with static key (rotation disabled)\n";
    } else {
      // Automatic key rotation mode (recommended for single-server deployments).
      // Keys are generated automatically and rotated at the configured interval.
      cfg.tls.withTlsSessionTickets(true)
          .withTlsSessionTicketLifetime(std::chrono::hours{2})
          .withTlsSessionTicketMaxKeys(4);
      std::cout << "Session tickets enabled with automatic key rotation\n";
      std::cout << "  Key lifetime: 2 hours\n";
      std::cout << "  Max keys in rotation: 4\n";
    }

    Router router;
    router.setDefault([](const HttpRequest& req) {
      HttpResponse resp(128U, http::StatusCodeOK);
      resp.bodyAppend("Hello from aeronet with TLS session tickets!\n");
      resp.bodyAppend("Path: ");
      resp.bodyAppend(req.path());
      resp.bodyAppend("\nTLS Version: ");
      resp.bodyAppend(req.tlsVersion());
      resp.bodyAppend("\nCipher: ");
      resp.bodyAppend(req.tlsCipher());
      resp.bodyAppend("\n");
      return resp;
    });

    SingleHttpServer server(std::move(cfg), std::move(router));

    std::cout << "Server listening on port " << port << "\n";
    std::cout << "\nTest session resumption:\n";
    std::cout << "  openssl s_client -connect localhost:" << port << " -sess_out session.pem\n";
    std::cout << "  openssl s_client -connect localhost:" << port << " -sess_in session.pem\n";
    std::cout << "  (Look for 'Reused, TLSv1.3' in output)\n\n";

    server.run();

    const auto stats = server.stats();
    std::cout << "\nServer stats:\n";
    std::cout << "  Total requests: " << stats.totalRequestsServed << '\n';
    std::cout << "  TLS handshakes: " << stats.tlsHandshakesSucceeded << '\n';
  } catch (const std::exception& ex) {
    std::cerr << "Error: " << ex.what() << '\n';
    return EXIT_FAILURE;
  }
}
