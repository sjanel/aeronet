// uwebsockets_server.cpp - uWebSockets benchmark server for WebSocket testing
//
// Implements the /ws echo endpoint plus minimal HTTP endpoints for health checks.
// Requires AERONET_BENCH_ENABLE_UWEBSOCKETS=ON during CMake configuration.
//
// NOTE: we deliberately avoid including scripted-servers-helpers.hpp here
// because it transitively includes aeronet/zlib-gateway.hpp (zlib-ng),
// which conflicts with the system zlib that uSockets links against.

#include <App.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

struct PerSocketData {};

namespace {

struct UwsConfig {
  uint16_t port;
  uint32_t numThreads;

  UwsConfig(uint16_t defaultPort, int argc, char* argv[])
      : port(defaultPort), numThreads(static_cast<uint32_t>(std::max(1u, std::thread::hardware_concurrency() / 2))) {
    const char* envPort = std::getenv("BENCH_PORT");
    if (envPort != nullptr) {
      port = static_cast<uint16_t>(std::atoi(envPort));
    }
    const char* envThreads = std::getenv("BENCH_THREADS");
    if (envThreads != nullptr) {
      numThreads = static_cast<uint32_t>(std::atoi(envThreads));
    }
    for (int ap = 1; ap < argc; ++ap) {
      std::string_view arg(argv[ap]);
      if (arg == "--port" && ap + 1 < argc) {
        port = static_cast<uint16_t>(std::atoi(argv[++ap]));
      } else if (arg == "--threads" && ap + 1 < argc) {
        numThreads = static_cast<uint32_t>(std::atoi(argv[++ap]));
      }
    }
  }
};

}  // namespace

int main(int argc, char* argv[]) {
  UwsConfig cfg(8088, argc, argv);

  uWS::App app;

  // ============================================================
  // Endpoint: /ping - Health check for benchmark runner
  // ============================================================
  app.get("/ping", [](auto* res, auto* /*req*/) { res->end("pong"); });

  // ============================================================
  // Endpoint: /status - JSON health check
  // ============================================================
  app.get("/status", [numThreads = cfg.numThreads](auto* res, auto* /*req*/) {
    res->writeHeader("Content-Type", "application/json");
    std::string body = R"({"server":"uwebsockets","threads":)" + std::to_string(numThreads) + R"(,"status":"ok"})";
    res->end(body);
  });

  // ============================================================
  // WebSocket: /ws - Echo endpoint for WebSocket benchmarks
  // Echoes back text and binary messages verbatim
  // ============================================================
  app.ws<PerSocketData>(
      "/ws", uWS::TemplatedApp<false>::WebSocketBehavior<PerSocketData>{
                 .compression = uWS::DISABLED,
                 .maxPayloadLength = 64 * 1024 * 1024,  // 64 MiB, matching aeronet default
                 .idleTimeout = 0,                      // No timeout for benchmarks
                 .maxBackpressure = 1024 * 1024,
                 .message = [](auto* ws, std::string_view message, uWS::OpCode opCode) { ws->send(message, opCode); },
             });

  app.listen(cfg.port, [port = cfg.port, numThreads = cfg.numThreads](auto* listenSocket) {
    if (listenSocket) {
      std::cout << "uWebSockets benchmark server starting on port " << port << " with " << numThreads << " threads\n";
      std::cout << "WebSocket echo endpoint registered at /ws\n";
    } else {
      std::cerr << "Failed to listen on port " << port << "\n";
      std::exit(1);
    }
  });

  app.run();

  return 0;
}
