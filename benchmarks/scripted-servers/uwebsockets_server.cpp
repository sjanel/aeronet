// uwebsockets_server.cpp - uWebSockets benchmark server for WebSocket testing
//
// Implements /ws-uncompressed and /ws-compressed echo endpoints plus minimal HTTP endpoints for health checks.
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
#include <vector>

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

void RunWorker(uint16_t port, uint32_t numThreads, bool printBanner) {
  uWS::App app;

  app.get("/ping", [](auto* res, auto* /*req*/) { res->end("pong"); });

  const std::string body = R"({"server":"uwebsockets","threads":)" + std::to_string(numThreads) + R"(,"status":"ok"})";
  app.get("/status", [body](auto* res, auto* /*req*/) {
    res->writeHeader("Content-Type", "application/json");
    res->end(body);
  });

  // /ws-uncompressed — no permessage-deflate, used by all non-compression scenarios
  app.ws<PerSocketData>("/ws-uncompressed", uWS::TemplatedApp<false>::WebSocketBehavior<PerSocketData>{
                                                .compression = uWS::DISABLED,
                                                .maxPayloadLength = 64 * 1024 * 1024,
                                                .idleTimeout = 0,
                                                .maxBackpressure = 1024 * 1024,
                                                .message = [](auto* ws, std::string_view message,
                                                              uWS::OpCode opCode) { ws->send(message, opCode); },
                                            });

  // /ws-compressed — permessage-deflate enabled, used by the compression scenario
  app.ws<PerSocketData>(
      "/ws-compressed",
      uWS::TemplatedApp<false>::WebSocketBehavior<PerSocketData>{
          .compression = static_cast<uWS::CompressOptions>(uWS::DEDICATED_COMPRESSOR | uWS::DEDICATED_DECOMPRESSOR),
          .maxPayloadLength = 64 * 1024 * 1024,
          .idleTimeout = 0,
          .maxBackpressure = 1024 * 1024,
          .message = [](auto* ws, std::string_view message, uWS::OpCode opCode) { ws->send(message, opCode, true); },
      });

  app.listen(port, [port, numThreads, printBanner](auto* listenSocket) {
    if (listenSocket) {
      if (printBanner) {
        std::cout << "uWebSockets benchmark server starting on port " << port << " with " << numThreads << " threads\n";
        std::cout << "WebSocket echo endpoints registered at /ws-uncompressed and /ws-compressed\n";
      }
    } else {
      std::cerr << "Failed to listen on port " << port << "\n";
      std::exit(1);
    }
  });

  app.run();
}

}  // namespace

int main(int argc, char* argv[]) {
  UwsConfig cfg(8088, argc, argv);

  // Spawn numThreads-1 background threads; the main thread acts as the last worker.
  // Each thread creates its own uWS::App and calls listen() on the same port —
  // uSockets sets SO_REUSEPORT so the OS load-balances incoming connections.
  std::vector<std::thread> threads;
  threads.reserve(cfg.numThreads - 1);
  for (uint32_t i = 1; i < cfg.numThreads; ++i) {
    threads.emplace_back([&cfg]() { RunWorker(cfg.port, cfg.numThreads, /*printBanner=*/false); });
  }

  RunWorker(cfg.port, cfg.numThreads, /*printBanner=*/true);

  for (auto& t : threads) {
    t.join();
  }

  return 0;
}
