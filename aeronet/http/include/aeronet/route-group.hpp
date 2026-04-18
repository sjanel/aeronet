#pragma once

#include <chrono>
#include <cstddef>
#include <string_view>

#include "aeronet/cors-policy.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/middleware.hpp"
#include "aeronet/path-handler-entry.hpp"
#include "aeronet/path-handlers.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/vector.hpp"

#ifdef AERONET_ENABLE_WEBSOCKET
#include "aeronet/websocket-endpoint.hpp"
#endif

namespace aeronet {

class Router;

/// Lightweight non-owning prefix proxy for Router.
///
/// Provides a scoped registration interface that prepends a common prefix to paths and
/// applies shared configuration (CORS, middleware, per-route limits) to all routes registered
/// through it. Per-route overrides applied after group registration take precedence.
///
/// A RouteGroup does NOT own the Router; the Router must outlive the group.
///
/// Example:
/// @code
///   auto api = router.group("/api/v1");
///   api.withMaxBodyBytes(65536).withCors(CorsPolicy(CorsPolicy::Active::On).allowAnyOrigin());
///   api.setPath(http::Method::GET, "/users", getUsers);    // registers "/api/v1/users"
///   api.setPath(http::Method::POST, "/users", postUsers);  // registers "/api/v1/users"
/// @endcode
class RouteGroup {
 public:
  RouteGroup(Router& router, std::string_view prefix);

#ifdef AERONET_ENABLE_HTTP2
  /// Set a shared HTTP/2 enable mode for all routes in this group.
  RouteGroup& withHttp2Enable(PathEntryConfig::Http2Enable mode) noexcept;
#endif

  /// Set a shared per-route maximum header size applied to all routes in this group.
  RouteGroup& withMaxHeaderBytes(uint32_t bytes) noexcept;

  /// Set a shared per-route maximum body size applied to all routes in this group.
  RouteGroup& withMaxBodyBytes(std::size_t bytes) noexcept;

  /// Set a shared per-route timeout applied to all routes in this group.
  RouteGroup& withTimeout(std::chrono::milliseconds ms);

  /// Set a shared CORS policy applied to all routes in this group.
  RouteGroup& withCors(CorsPolicy policy);

  /// Add a shared request middleware applied before each route handler in this group.
  RouteGroup& addRequestMiddleware(RequestMiddleware middleware);

  /// Add a shared response middleware applied after each route handler in this group.
  RouteGroup& addResponseMiddleware(ResponseMiddleware middleware);

  /// Register a request handler at prefix + subpath.
  PathHandlerEntry& setPath(http::MethodBmp methods, std::string_view subpath, RequestHandler handler);

  /// Register a request handler at prefix + subpath (single method).
  PathHandlerEntry& setPath(http::Method method, std::string_view subpath, RequestHandler handler);

  /// Register a streaming handler at prefix + subpath.
  PathHandlerEntry& setPath(http::MethodBmp methods, std::string_view subpath, StreamingHandler handler);

  /// Register a streaming handler at prefix + subpath (single method).
  PathHandlerEntry& setPath(http::Method method, std::string_view subpath, StreamingHandler handler);

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  /// Register an async handler at prefix + subpath.
  PathHandlerEntry& setPath(http::MethodBmp methods, std::string_view subpath, AsyncRequestHandler handler);

  /// Register an async handler at prefix + subpath (single method).
  PathHandlerEntry& setPath(http::Method method, std::string_view subpath, AsyncRequestHandler handler);
#endif

#ifdef AERONET_ENABLE_WEBSOCKET
  /// Register a WebSocket endpoint at prefix + subpath.
  PathHandlerEntry& setWebSocket(std::string_view subpath, WebSocketEndpoint endpoint);
#endif

  /// Create a nested child group with an additional sub-prefix.
  /// The child inherits this group's config and middleware.
  [[nodiscard]] RouteGroup group(std::string_view subprefix) const;

 private:
  void applyGroupConfig(PathHandlerEntry& entry) const;

  Router* _router;
  RawChars32 _prefix;
  std::chrono::milliseconds _timeout = std::chrono::milliseconds::max();
  std::size_t _maxBodyBytes = static_cast<std::size_t>(-1);
  uint32_t _maxHeaderBytes = static_cast<uint32_t>(-1);
#ifdef AERONET_ENABLE_HTTP2
  PathEntryConfig::Http2Enable _http2Enable{PathEntryConfig::Http2Enable::Default};
#endif
  CorsPolicy _corsPolicy;
  vector<RequestMiddleware> _preMiddleware;
  vector<ResponseMiddleware> _postMiddleware;
};

}  // namespace aeronet
