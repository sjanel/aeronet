#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
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
struct PathEntryConfig {
  /// Maximum timeout representable by the internal uint32_t millisecond storage.
  /// The absolute maximum (~49.7 days) is reserved as a sentinel; configured values must not exceed this.
  static constexpr auto kMaxRequestTimeout = std::chrono::milliseconds(std::numeric_limits<uint32_t>::max() - 1);

#ifdef AERONET_ENABLE_HTTP2
  /// HTTP/2 enable mode for this specific path.
  ///
  /// - Default: Use the global Http2Config.enable setting from HttpServerConfig
  /// - Enable: Force HTTP/2 support for this path (if client supports it)
  /// - Disable: Force HTTP/1.1 only for this path even if HTTP/2 is globally enabled
  enum class Http2Enable : uint8_t { Default, Enable, Disable };

  Http2Enable http2Enable{Http2Enable::Default};
#endif

  /// Per-route maximum header size in bytes. Must not exceed HttpServerConfig::maxHeaderBytes.
  /// Default (sentinel) means "use the global limit".
  uint32_t maxHeaderBytes = static_cast<uint32_t>(-1);

  /// Per-route maximum body size in bytes. Must not exceed HttpServerConfig::maxBodyBytes.
  /// Default (sentinel) means "use the global limit".
  std::size_t maxBodyBytes = static_cast<std::size_t>(-1);

  /// Per-route handler deadline. std::chrono::milliseconds::max() means use the global timeout (or no limit).
  std::chrono::milliseconds requestTimeout = std::chrono::milliseconds::max();
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

#ifdef AERONET_ENABLE_HTTP2
  /// Configure whether HTTP/2 is allowed for this route.
  ///
  /// Default: follow global HTTP/2 setting.
  /// Enable: force HTTP/2 support for this route.
  /// Disable: force HTTP/1.1 only for this route.
  PathHandlerEntry& http2Enable(PathEntryConfig::Http2Enable mode) noexcept {
    _pathConfig.http2Enable = mode;
    return *this;
  }
#endif

  /// Set a per-route maximum header size in bytes. Overrides the global maxHeaderBytes for this route.
  PathHandlerEntry& maxHeaderBytes(uint32_t bytes) noexcept {
    _pathConfig.maxHeaderBytes = bytes;
    return *this;
  }

  /// Set a per-route maximum body size in bytes. Overrides the global maxBodyBytes for this route.
  PathHandlerEntry& maxBodyBytes(std::size_t bytes) noexcept {
    _pathConfig.maxBodyBytes = bytes;
    return *this;
  }

  /// Set a per-route handler deadline. Overrides the global timeout for this route.
  PathHandlerEntry& timeout(std::chrono::milliseconds ms) {
    if (ms != std::chrono::milliseconds::max() && ms > PathEntryConfig::kMaxRequestTimeout) {
      throw std::invalid_argument("per-route requestTimeout exceeds maximum representable duration (~49.7 days)");
    }
    _pathConfig.requestTimeout = ms;
    return *this;
  }

 private:
  friend class Router;
  friend class SingleHttpServer;
  friend class PathHandlerEntryTest;

  struct HandlerStorage {
    static_assert(sizeof(RequestHandler) == sizeof(StreamingHandler));

    static_assert(std::alignment_of_v<RequestHandler> == std::alignment_of_v<StreamingHandler>);
#ifdef AERONET_ENABLE_ASYNC_HANDLERS
    static_assert(sizeof(RequestHandler) == sizeof(AsyncRequestHandler));
    static_assert(std::alignment_of_v<RequestHandler> == std::alignment_of_v<AsyncRequestHandler>);
#endif

    alignas(RequestHandler) std::byte normalHandlerStorage[sizeof(RequestHandler)];
  };

  PathHandlerEntry() noexcept = default;

  void assignNormalHandler(http::MethodBmp methodBmp, RequestHandler handler);
#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  void assignAsyncHandler(http::MethodBmp methodBmp, AsyncRequestHandler handler);
#endif
  void assignStreamingHandler(http::MethodBmp methodBmp, StreamingHandler handler);

  [[nodiscard]] bool hasNormalHandler(http::MethodIdx methodIdx) const {
    return http::IsMethodIdxSet(_normalMethodBmp, methodIdx);
  }

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  [[nodiscard]] bool hasAsyncHandler(http::MethodIdx methodIdx) const {
    return http::IsMethodIdxSet(_asyncMethodBmp, methodIdx);
  }
#endif

  [[nodiscard]] bool hasStreamingHandler(http::MethodIdx methodIdx) const {
    return http::IsMethodIdxSet(_streamingMethodBmp, methodIdx);
  }

  [[nodiscard]] const RequestHandler* requestHandlerPtr(http::MethodIdx methodIdx) const {
    return &reinterpret_cast<const RequestHandler&>(_handlers[methodIdx]);
  }

  [[nodiscard]] const StreamingHandler* streamingHandlerPtr(http::MethodIdx methodIdx) const {
    return &reinterpret_cast<const StreamingHandler&>(_handlers[methodIdx]);
  }

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  [[nodiscard]] const AsyncRequestHandler* asyncHandlerPtr(http::MethodIdx methodIdx) const {
    return &reinterpret_cast<const AsyncRequestHandler&>(_handlers[methodIdx]);
  }
#endif

#ifdef AERONET_ENABLE_WEBSOCKET
  void assignWebSocketEndpoint(WebSocketEndpoint endpoint);

  [[nodiscard]] bool hasWebSocketEndpoint() const { return _websocketEndpoint != nullptr; }

  [[nodiscard]] const WebSocketEndpoint* webSocketEndpointPtr() const { return _websocketEndpoint.get(); }
#endif

  /// Check if this entry has any handlers (HTTP or WebSocket).
  [[nodiscard]] bool hasAnyHandler() const {
#ifdef AERONET_ENABLE_WEBSOCKET
    return _normalMethodBmp != 0U || _streamingMethodBmp != 0U
#ifdef AERONET_ENABLE_ASYNC_HANDLERS
           || _asyncMethodBmp != 0U
#endif
           || hasWebSocketEndpoint();
#else
    return _normalMethodBmp != 0U || _streamingMethodBmp != 0U
#ifdef AERONET_ENABLE_ASYNC_HANDLERS
           || _asyncMethodBmp != 0U
#endif
        ;
#endif
  }

  void destroyIdx(http::MethodIdx methodIdx);

  http::MethodBmp _normalMethodBmp{};
  http::MethodBmp _streamingMethodBmp{};
#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  http::MethodBmp _asyncMethodBmp{};
#endif
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
