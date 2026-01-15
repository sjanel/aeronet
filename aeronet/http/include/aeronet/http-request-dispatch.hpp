#pragma once

#include <optional>
#include <span>
#include <string_view>

#include "aeronet/cors-policy.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/middleware.hpp"
#include "aeronet/router.hpp"
#include "aeronet/tracing/tracer.hpp"

namespace aeronet {

/// Configuration for special method processing.
struct SpecialMethodConfig {
  HttpServerConfig::TraceMethodPolicy tracePolicy{HttpServerConfig::TraceMethodPolicy::Disabled};
  bool isTls{false};  ///< true if request arrived over TLS (for TRACE policy check)
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
[[nodiscard]] std::optional<HttpResponse> ProcessSpecialMethods(const HttpRequest& request, Router& router,
                                                                const SpecialMethodConfig& config,
                                                                const CorsPolicy* pCorsPolicy,
                                                                std::string_view requestData = {});

/// Run a chain of request middleware.
/// Executes each middleware in order until one short-circuits or all complete.
///
/// @param request Request being processed (may be modified by middleware)
/// @param chain Middleware chain to execute
/// @return Result indicating if processing was short-circuited
[[nodiscard]] std::optional<HttpResponse> RunRequestMiddleware(HttpRequest& request,
                                                               std::span<const RequestMiddleware> global,
                                                               std::span<const RequestMiddleware> chain,
                                                               const tracing::TelemetryContext& telemetryContext,
                                                               bool streaming,
                                                               const MiddlewareMetricsCallback& metricsCallback);

/// Apply response middleware chain to a response.
/// @param request Original request (read-only for middleware)
/// @param response Response to modify
/// @param chain Response middleware chain to execute
void ApplyResponseMiddleware(const HttpRequest& request, HttpResponse& response,
                             std::span<const ResponseMiddleware> chain,
                             std::span<const ResponseMiddleware> globalMiddleware,
                             const tracing::TelemetryContext& telemetryContext, bool streaming,
                             const MiddlewareMetricsCallback& metricsCallback);

}  // namespace aeronet
