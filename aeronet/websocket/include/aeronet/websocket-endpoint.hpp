#pragma once

#include <functional>
#include <memory>
#include <string>

#include "aeronet/concatenated-strings.hpp"
#include "aeronet/websocket-handler.hpp"

namespace aeronet {

class HttpRequest;

/// Factory function that creates a WebSocketHandler for a new connection.
/// Receives the upgrade request to allow per-connection customization.
/// The factory should configure callbacks before returning the handler.
using WebSocketHandlerFactory = std::function<std::unique_ptr<websocket::WebSocketHandler>(const HttpRequest&)>;

/// Simplified WebSocket endpoint that uses default configuration.
/// Receives callbacks that will be invoked for all connections on this endpoint.
struct WebSocketEndpoint {
  /// Configuration for WebSocket connections on this endpoint.
  websocket::WebSocketConfig config;

  /// Factory function to create handlers for new connections.
  /// If not set, uses CreateServerWebSocketHandler with default config.
  WebSocketHandlerFactory factory;

  /// Subprotocols supported by this endpoint, in order of preference.
  /// If the client offers one of these, the first matching one is selected.
  /// Common examples: "graphql-ws", "graphql-transport-ws", "chat", "v1.json"
  ConcatenatedStrings supportedProtocols;

  /// Create an endpoint with a custom handler factory.
  static WebSocketEndpoint WithFactory(WebSocketHandlerFactory factory) {
    WebSocketEndpoint ep;
    ep.factory = std::move(factory);
    return ep;
  }

  /// Create an endpoint with callbacks shared across all connections.
  /// This is the simplest way to create a WebSocket endpoint.
  static WebSocketEndpoint WithCallbacks(websocket::WebSocketCallbacks callbacks) {
    WebSocketEndpoint ep;
    ep.factory = [cb = std::move(callbacks)](const HttpRequest& /*request*/) mutable {
      return std::make_unique<websocket::WebSocketHandler>(websocket::WebSocketConfig{}, std::move(cb));
    };
    return ep;
  }

  /// Create an endpoint with config and callbacks.
  static WebSocketEndpoint WithConfigAndCallbacks(websocket::WebSocketConfig config,
                                                  websocket::WebSocketCallbacks callbacks) {
    WebSocketEndpoint ep;
    ep.config = config;
    ep.factory = [cfg = config, cb = std::move(callbacks)](const HttpRequest& /*request*/) mutable {
      return std::make_unique<websocket::WebSocketHandler>(cfg, std::move(cb));
    };
    return ep;
  }

  /// Create an endpoint with subprotocols and callbacks.
  /// @param protocols  Subprotocols supported by this endpoint, in preference order
  /// @param callbacks  Callbacks for WebSocket events
  static WebSocketEndpoint WithProtocolsAndCallbacks(std::span<const std::string> protocols,
                                                     websocket::WebSocketCallbacks callbacks) {
    WebSocketEndpoint ep;
    for (const auto& proto : protocols) {
      ep.supportedProtocols.append(proto);
    }
    ep.factory = [cb = std::move(callbacks)](const HttpRequest& /*request*/) mutable {
      return std::make_unique<websocket::WebSocketHandler>(websocket::WebSocketConfig{}, std::move(cb));
    };
    return ep;
  }

  /// Create a fully configured endpoint.
  /// @param config     WebSocket configuration
  /// @param protocols  Subprotocols supported, in preference order
  /// @param callbacks  Callbacks for WebSocket events
  static WebSocketEndpoint WithFullConfig(websocket::WebSocketConfig config, std::span<const std::string> protocols,
                                          websocket::WebSocketCallbacks callbacks) {
    WebSocketEndpoint ep;
    ep.config = config;
    for (const auto& proto : protocols) {
      ep.supportedProtocols.append(proto);
    }
    ep.factory = [cfg = config, cb = std::move(callbacks)](const HttpRequest& /*request*/) mutable {
      return std::make_unique<websocket::WebSocketHandler>(cfg, std::move(cb));
    };
    return ep;
  }
};

}  // namespace aeronet
