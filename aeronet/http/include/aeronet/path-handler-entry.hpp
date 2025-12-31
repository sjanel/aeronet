#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "aeronet/cors-policy.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/middleware.hpp"
#include "aeronet/path-handlers.hpp"
#include "aeronet/vector.hpp"

#ifdef AERONET_ENABLE_WEBSOCKET
#include <memory>

#include "aeronet/websocket-endpoint.hpp"
#endif

namespace aeronet {

/// Per-path configuration options for route handlers.
///
/// This struct allows fine-grained control over routing behavior on a per-path basis.
/// Pass an instance to Router::setPath() or Router::setDefault() to configure specific
/// options for that route.
///
/// Example:
/// @code
///   router.setPath(http::Method::GET, "/api/v2/stream",
///                  myHandler, PathEntryConfig{.http2Enable = PathEntryConfig::Http2Enable::Enable});
/// @endcode
struct PathEntryConfig {
  /// HTTP/2 enable mode for this specific path.
  ///
  /// - Default: Use the global Http2Config.enable setting from HttpServerConfig
  /// - Enable: Force HTTP/2 support for this path (if client supports it)
  /// - Disable: Force HTTP/1.1 only for this path even if HTTP/2 is globally enabled
  enum class Http2Enable : uint8_t { Default, Enable, Disable };

  Http2Enable http2Enable{Http2Enable::Default};
};

// Object that stores handlers and options for a specific group of paths.
class PathHandlerEntry {
 public:
  PathHandlerEntry(const PathHandlerEntry& rhs);
  PathHandlerEntry(PathHandlerEntry&& rhs) noexcept;

  PathHandlerEntry& operator=(const PathHandlerEntry& rhs);
  PathHandlerEntry& operator=(PathHandlerEntry&& rhs) noexcept;

  ~PathHandlerEntry();

  // Attach given corsPolicy to the path handler entry.
  PathHandlerEntry& cors(CorsPolicy corsPolicy);

  // Register middleware executed before the route handler. The middleware may mutate
  // the request and short-circuit the chain by returning a response.
  PathHandlerEntry& before(RequestMiddleware middleware);

  // Register middleware executed after the route handler produces a response. The middleware
  // can amend headers or body before the response is finalized.
  PathHandlerEntry& after(ResponseMiddleware middleware);

  /// Configure whether HTTP/2 is allowed for this route.
  ///
  /// Default: follow global HTTP/2 setting.
  /// Enable: force HTTP/2 support for this route.
  /// Disable: force HTTP/1.1 only for this route.
  PathHandlerEntry& http2Enable(PathEntryConfig::Http2Enable mode) noexcept {
    _pathConfig.http2Enable = mode;
    return *this;
  }

 private:
  friend class Router;
  friend class SingleHttpServer;
  friend class PathHandlerEntryTest;

  struct HandlerStorage {
    static_assert(sizeof(RequestHandler) == sizeof(AsyncRequestHandler));
    static_assert(sizeof(RequestHandler) == sizeof(StreamingHandler));

    static_assert(std::alignment_of_v<RequestHandler> == std::alignment_of_v<AsyncRequestHandler>);
    static_assert(std::alignment_of_v<RequestHandler> == std::alignment_of_v<StreamingHandler>);

    alignas(RequestHandler) std::byte normalHandlerStorage[sizeof(RequestHandler)];
  };

  PathHandlerEntry() noexcept = default;

  void assignNormalHandler(http::MethodBmp methodBmp, RequestHandler handler);
  void assignAsyncHandler(http::MethodBmp methodBmp, AsyncRequestHandler handler);
  void assignStreamingHandler(http::MethodBmp methodBmp, StreamingHandler handler);

  [[nodiscard]] bool hasNormalHandler(http::MethodIdx methodIdx) const {
    return http::IsMethodIdxSet(_normalMethodBmp, methodIdx);
  }

  [[nodiscard]] bool hasAsyncHandler(http::MethodIdx methodIdx) const {
    return http::IsMethodIdxSet(_asyncMethodBmp, methodIdx);
  }

  [[nodiscard]] bool hasStreamingHandler(http::MethodIdx methodIdx) const {
    return http::IsMethodIdxSet(_streamingMethodBmp, methodIdx);
  }

  [[nodiscard]] const RequestHandler* requestHandlerPtr(http::MethodIdx methodIdx) const {
    return &reinterpret_cast<const RequestHandler&>(_handlers[methodIdx]);
  }

  [[nodiscard]] const StreamingHandler* streamingHandlerPtr(http::MethodIdx methodIdx) const {
    return &reinterpret_cast<const StreamingHandler&>(_handlers[methodIdx]);
  }

  [[nodiscard]] const AsyncRequestHandler* asyncHandlerPtr(http::MethodIdx methodIdx) const {
    return &reinterpret_cast<const AsyncRequestHandler&>(_handlers[methodIdx]);
  }

#ifdef AERONET_ENABLE_WEBSOCKET
  void assignWebSocketEndpoint(WebSocketEndpoint endpoint);

  [[nodiscard]] bool hasWebSocketEndpoint() const { return _websocketEndpoint != nullptr; }

  [[nodiscard]] const WebSocketEndpoint* webSocketEndpointPtr() const { return _websocketEndpoint.get(); }
#endif

  /// Check if this entry has any handlers (HTTP or WebSocket).
  [[nodiscard]] bool hasAnyHandler() const {
#ifdef AERONET_ENABLE_WEBSOCKET
    return _normalMethodBmp != 0U || _streamingMethodBmp != 0U || _asyncMethodBmp != 0U || hasWebSocketEndpoint();
#else
    return _normalMethodBmp != 0U || _streamingMethodBmp != 0U || _asyncMethodBmp != 0U;
#endif
  }

  void destroyIdx(http::MethodIdx methodIdx);

  http::MethodBmp _normalMethodBmp{};
  http::MethodBmp _streamingMethodBmp{};
  http::MethodBmp _asyncMethodBmp{};
  std::array<HandlerStorage, http::kNbMethods> _handlers;
#ifdef AERONET_ENABLE_WEBSOCKET
  // Optional WebSocket endpoint for this route. If set, upgrade requests are handled here.
  std::unique_ptr<WebSocketEndpoint> _websocketEndpoint;
#endif
  // Optional per-route CorsPolicy stored by value. If set, match() will return a pointer to it.
  CorsPolicy _corsPolicy;
  vector<RequestMiddleware> _preMiddleware;
  vector<ResponseMiddleware> _postMiddleware;
  // Per-path configuration for HTTP/2 and other options.
  PathEntryConfig _pathConfig;
};

}  // namespace aeronet
