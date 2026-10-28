#pragma once

#include <span>
#include <string_view>

#include "aeronet/cors-policy.hpp"
#include "aeronet/http-request-view.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/middleware.hpp"
#include "aeronet/path-handlers.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/router.hpp"
#include "aeronet/tracing/tracer.hpp"

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
#include "aeronet/request-task.hpp"
#endif

namespace aeronet {

/// Configuration for special method processing.
struct SpecialMethodConfig {
  HttpServerConfig::TraceMethodPolicy tracePolicy{HttpServerConfig::TraceMethodPolicy::Disabled};
  bool isTls{false};  // true if request arrived over TLS (for TRACE policy check)
};

/// Process special HTTP methods (OPTIONS, TRACE) in a protocol-agnostic way.
///
/// This function handles:
/// - OPTIONS: Returns Allow header with supported methods, handles CORS preflight
/// - TRACE: Echoes request if policy allows
///
/// Note: CONNECT is NOT handled here as it requires protocol-specific tunnel setup.
/// HTTP/1.1 and HTTP/2 must handle CONNECT differently due to transport semantics.
///
/// @param request The incoming HTTP request
/// @param router Router for method allow-list lookup (non-const: allowedMethods mutates internal buffer)
/// @param config Special method configuration
/// @param pCorsPolicy Optional CORS policy (can be nullptr)
/// @param requestData Raw request data for TRACE echo (only needed for HTTP/1.1 TRACE)
/// @return Result indicating action taken
[[nodiscard]] HttpResponse ProcessSpecialMethods(const HttpRequestView& request, Router& router,
                                                 SpecialMethodConfig config, const CorsPolicy* pCorsPolicy,
                                                 std::string_view requestData = {});

/// The steps of the request pipeline shared by HTTP/1.1 and HTTP/2, so that both protocols answer a request the same
/// way. Once a request is parsed and routed, and its protocol-specific checks and special methods are handled, the
/// protocol:
///  1. runs its request middleware with runRequestMiddleware(),
///  2. unless a middleware answered, calls callRequestHandler(), which answers the request unless the protocol has to
///     call the streaming or the async handler of its route,
///  3. runs the response middleware on the response with applyResponseMiddleware(), then applies the CORS policy of
///     the route while finalizing it. The head of a streaming response goes through prepareStreamingHead() instead.
/// It points to objects owned by the server, and is cheap to copy.
class RequestPipeline {
 public:
  RequestPipeline(Router& router, tracing::TelemetryContext& telemetry,
                  const MiddlewareMetricsCallback& middlewareMetrics, RawChars& scratch) noexcept
      : _pRouter(&router), _pTelemetry(&telemetry), _pMiddlewareMetrics(&middlewareMetrics), _pScratch(&scratch) {}

  /// Install the path and query parameters of a routed request, then run its request middleware: the global chain,
  /// then the chain of its route.
  /// Returns the response of a middleware answering instead of the handler (a short-circuit, or a 500 when one throws).
  [[nodiscard]] HttpResponse runRequestMiddleware(HttpRequestView& request, const Router::RoutingResult& routingResult,
                                                  bool streaming) const;

  /// Answer a request that went through its request middleware: a 403 when the CORS policy of its route denies its
  /// origin (its handler is not called, so that it has no side effect), the response of its request handler (a 500 when
  /// it throws), or, without handler, a trailing slash redirect, a 405 or a 404.
  /// Returns nothing when the protocol calls the handler of the route: its streaming handler (when 'streaming') or its
  /// async handler.
  [[nodiscard]] HttpResponse callRequestHandler(HttpRequestView& request, const Router::RoutingResult& routingResult,
                                                bool streaming) const;

  /// Tell whether the CORS policy of the route of a request denies its origin: its handler must then not be called, the
  /// request being answered by MakeOriginDenial().
  [[nodiscard]] static bool IsOriginDenied(const HttpRequestView& request,
                                           const Router::RoutingResult& routingResult) noexcept {
    const CorsPolicy* pCorsPolicy = routingResult.corsPolicy();
    return pCorsPolicy != nullptr && pCorsPolicy->wouldApply(request) == CorsPolicy::ApplyStatus::OriginDenied;
  }

  /// The 403 answering a request whose origin is denied by the CORS policy of its route (see IsOriginDenied()).
  [[nodiscard]] static HttpResponse MakeOriginDenial(const HttpRequestView& request);

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  /// Call an async handler, which creates the task of its coroutine (not started yet). Returns an invalid task when the
  /// handler throws (a function returning a RequestTask without being a coroutine) or returns one, logged: the protocol
  /// then answers 500.
  [[nodiscard]] static RequestTask<HttpResponse> CallAsyncHandler(const AsyncRequestHandler& handler,
                                                                  HttpRequestView& request);
#endif

  /// Run the response middleware on a response: the chain of its route, then the global chain.
  void applyResponseMiddleware(const HttpRequestView& request, HttpResponse& response,
                               std::span<const ResponseMiddleware> routeChain, bool streaming) const;

  /// Prepare the head of a streaming response, sent before its body: run the response middleware, then apply the CORS
  /// policy of its route (if any).
  void prepareStreamingHead(const HttpRequestView& request, HttpResponse& head,
                            std::span<const ResponseMiddleware> routeChain, const CorsPolicy* pCorsPolicy) const;

  [[nodiscard]] Router& router() const noexcept { return *_pRouter; }

  [[nodiscard]] tracing::TelemetryContext& telemetry() const noexcept { return *_pTelemetry; }

 private:
  [[nodiscard]] HttpResponse makeTrailingSlashRedirect(
      const HttpRequestView& request, Router::RoutingResult::RedirectSlashMode redirectSlashMode) const;

  Router* _pRouter;
  tracing::TelemetryContext* _pTelemetry;
  const MiddlewareMetricsCallback* _pMiddlewareMetrics;
  RawChars* _pScratch;
};

}  // namespace aeronet
