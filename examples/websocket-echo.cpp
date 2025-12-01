#include <aeronet/aeronet.hpp>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

using namespace aeronet;

namespace {

/// Convert bytes to string_view for display
std::string_view PayloadToString(std::span<const std::byte> payload) {
  return {reinterpret_cast<const char*>(payload.data()), payload.size()};
}

}  // namespace

int main(int argc, char** argv) {
  uint16_t port = 0;
  if (argc > 1) {
    port = static_cast<uint16_t>(std::stoi(argv[1]));
  }

  // Enable signal handler for graceful shutdown on Ctrl+C
  SignalHandler::Enable();

  Router router;

  // Add a simple HTTP endpoint for testing
  router.setPath(http::Method::GET, "/", [](const HttpRequest& /*req*/) {
    return HttpResponse(R"html(
<!DOCTYPE html>
<html>
<head><title>WebSocket Echo</title></head>
<body>
<h1>WebSocket Echo Server</h1>
<p>Connect to <code>ws://localhost:PORT/ws</code></p>
<input id="msg" type="text" placeholder="Message to send">
<button onclick="send()">Send</button>
<div id="log" style="font-family:monospace;white-space:pre;"></div>
<script>
const ws = new WebSocket('ws://' + location.host + '/ws');
ws.onopen = () => log('Connected');
ws.onmessage = e => log('Received: ' + e.data);
ws.onclose = e => log('Closed: ' + e.code + ' ' + e.reason);
ws.onerror = () => log('Error');
function send() {
  const msg = document.getElementById('msg').value;
  ws.send(msg);
  log('Sent: ' + msg);
}
function log(s) { document.getElementById('log').textContent += s + '\n'; }
</script>
</body>
</html>
)html",
                        "text/html");
  });

  // Configure WebSocket callbacks
  websocket::WebSocketCallbacks callbacks{
      .onMessage =
          [](std::span<const std::byte> payload, bool isBinary) {
            // This callback doesn't have access to the handler directly,
            // so we need to use the factory pattern for echo functionality
            std::cout << "[WS] Message received (" << (isBinary ? "binary" : "text") << ", " << payload.size()
                      << " bytes): " << PayloadToString(payload) << "\n";
          },
      .onPing =
          [](std::span<const std::byte> payload) {
            std::cout << "[WS] Ping received (" << payload.size() << " bytes)\n";
          },
      .onPong =
          [](std::span<const std::byte> payload) {
            std::cout << "[WS] Pong received (" << payload.size() << " bytes)\n";
          },
      .onClose =
          [](websocket::CloseCode code, std::string_view reason) {
            std::cout << "[WS] Close received: " << static_cast<uint16_t>(code) << " - " << reason << "\n";
          },
      .onError =
          [](websocket::CloseCode code, std::string_view message) {
            std::cerr << "[WS] Error: " << static_cast<uint16_t>(code) << " - " << message << "\n";
          },
  };

  // Register WebSocket endpoint with factory for echo functionality
  // Using factory pattern allows access to the handler for sending responses
  router.setWebSocket(
      "/ws", WebSocketEndpoint::WithFactory([](const HttpRequest& req) {
        std::cout << "[WS] New connection from path: " << req.path() << "\n";

        auto handler = std::make_unique<websocket::WebSocketHandler>();

        // Capture raw pointer before moving handler
        websocket::WebSocketHandler* handlerPtr = handler.get();

        handler->setCallbacks(websocket::WebSocketCallbacks{
            .onMessage =
                [handlerPtr](std::span<const std::byte> payload, bool isBinary) {
                  auto msg = PayloadToString(payload);
                  std::cout << "[WS] Received (" << (isBinary ? "binary" : "text") << "): " << msg << "\n";

                  // Echo back
                  if (isBinary) {
                    handlerPtr->sendBinary(payload);
                  } else {
                    handlerPtr->sendText(msg);
                  }
                  std::cout << "[WS] Echoed back\n";
                },
            .onPing =
                [](std::span<const std::byte> payload) {
                  std::cout << "[WS] Ping (" << payload.size() << " bytes) - pong sent automatically\n";
                },
            .onPong =
                [](std::span<const std::byte> payload) { std::cout << "[WS] Pong (" << payload.size() << " bytes)\n"; },
            .onClose =
                [](websocket::CloseCode code, std::string_view reason) {
                  std::cout << "[WS] Connection closing: " << static_cast<uint16_t>(code) << " - " << reason << "\n";
                },
            .onError =
                [](websocket::CloseCode code, std::string_view message) {
                  std::cerr << "[WS] Protocol error: " << static_cast<uint16_t>(code) << " - " << message << "\n";
                },
        });

        return handler;
      }));

  try {
    HttpServerConfig config;
    config.withPort(port);

    HttpServer server(config, std::move(router));

    std::cout << "WebSocket echo server listening on port " << server.port() << "\n";
    std::cout << "  HTTP page:     http://localhost:" << server.port() << "/\n";
    std::cout << "  WebSocket:     ws://localhost:" << server.port() << "/ws\n";
    std::cout << "Press Ctrl+C to stop\n";

    server.run();
  } catch (const std::exception& ex) {
    std::cerr << "Error: " << ex.what() << '\n';
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
