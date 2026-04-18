#include "aeronet/route-group.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "aeronet/cors-policy.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/middleware.hpp"
#include "aeronet/path-handler-entry.hpp"
#include "aeronet/path-handlers.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/router.hpp"

#ifdef AERONET_ENABLE_WEBSOCKET
#include "aeronet/websocket-endpoint.hpp"
#endif

namespace aeronet {

namespace {

RawChars FullPath(std::string_view prefix, std::string_view subpath) {
  RawChars fullPath(prefix.size() + subpath.size());
  fullPath.append(prefix);
  fullPath.append(subpath);
  return fullPath;
}

}  // namespace

RouteGroup::RouteGroup(Router& router, std::string_view prefix) : _router(&router), _prefix(prefix) {}

RouteGroup& RouteGroup::withMaxHeaderBytes(uint32_t bytes) noexcept {
  _maxHeaderBytes = bytes;
  return *this;
}

RouteGroup& RouteGroup::withMaxBodyBytes(std::size_t bytes) noexcept {
  _maxBodyBytes = bytes;
  return *this;
}

RouteGroup& RouteGroup::withTimeout(std::chrono::milliseconds ms) {
  if (ms != std::chrono::milliseconds::max() && ms > PathEntryConfig::kMaxRequestTimeout) {
    throw std::invalid_argument("per-route group timeout exceeds maximum representable duration (~49.7 days)");
  }
  _timeout = ms;
  return *this;
}

#ifdef AERONET_ENABLE_HTTP2
RouteGroup& RouteGroup::withHttp2Enable(PathEntryConfig::Http2Enable mode) noexcept {
  _http2Enable = mode;
  return *this;
}
#endif

RouteGroup& RouteGroup::withCors(CorsPolicy policy) {
  _corsPolicy = std::move(policy);
  return *this;
}

RouteGroup& RouteGroup::addRequestMiddleware(RequestMiddleware middleware) {
  _preMiddleware.push_back(std::move(middleware));
  return *this;
}

RouteGroup& RouteGroup::addResponseMiddleware(ResponseMiddleware middleware) {
  _postMiddleware.push_back(std::move(middleware));
  return *this;
}

PathHandlerEntry& RouteGroup::setPath(http::MethodBmp methods, std::string_view subpath, RequestHandler handler) {
  auto& entry = _router->setPath(methods, FullPath(_prefix, subpath), std::move(handler));
  applyGroupConfig(entry);
  return entry;
}

PathHandlerEntry& RouteGroup::setPath(http::Method method, std::string_view subpath, RequestHandler handler) {
  return setPath(static_cast<http::MethodBmp>(method), subpath, std::move(handler));
}

PathHandlerEntry& RouteGroup::setPath(http::MethodBmp methods, std::string_view subpath, StreamingHandler handler) {
  auto& entry = _router->setPath(methods, FullPath(_prefix, subpath), std::move(handler));
  applyGroupConfig(entry);
  return entry;
}

PathHandlerEntry& RouteGroup::setPath(http::Method method, std::string_view subpath, StreamingHandler handler) {
  return setPath(static_cast<http::MethodBmp>(method), subpath, std::move(handler));
}

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
PathHandlerEntry& RouteGroup::setPath(http::MethodBmp methods, std::string_view subpath, AsyncRequestHandler handler) {
  auto& entry = _router->setPath(methods, FullPath(_prefix, subpath), std::move(handler));
  applyGroupConfig(entry);
  return entry;
}

PathHandlerEntry& RouteGroup::setPath(http::Method method, std::string_view subpath, AsyncRequestHandler handler) {
  return setPath(static_cast<http::MethodBmp>(method), subpath, std::move(handler));
}
#endif

#ifdef AERONET_ENABLE_WEBSOCKET
PathHandlerEntry& RouteGroup::setWebSocket(std::string_view subpath, WebSocketEndpoint endpoint) {
  auto& entry = _router->setWebSocket(FullPath(_prefix, subpath), std::move(endpoint));
  applyGroupConfig(entry);
  return entry;
}
#endif

RouteGroup RouteGroup::group(std::string_view subprefix) const {
  RouteGroup child(*_router, FullPath(_prefix, subprefix));
  child._timeout = _timeout;
  child._maxBodyBytes = _maxBodyBytes;
  child._maxHeaderBytes = _maxHeaderBytes;
#ifdef AERONET_ENABLE_HTTP2
  child._http2Enable = _http2Enable;
#endif
  child._corsPolicy = _corsPolicy;
  child._preMiddleware = _preMiddleware;
  child._postMiddleware = _postMiddleware;
  return child;
}

void RouteGroup::applyGroupConfig(PathHandlerEntry& entry) const {
  entry.cors(_corsPolicy);
  for (const auto& mw : _preMiddleware) {
    entry.before(mw);
  }
  for (const auto& mw : _postMiddleware) {
    entry.after(mw);
  }
#ifdef AERONET_ENABLE_HTTP2
  if (_http2Enable != PathEntryConfig::Http2Enable::Default) {
    entry.http2Enable(_http2Enable);
  }
#endif
  entry.maxHeaderBytes(_maxHeaderBytes);
  entry.maxBodyBytes(_maxBodyBytes);
  entry.timeout(_timeout);
}

}  // namespace aeronet
