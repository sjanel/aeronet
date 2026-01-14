#include "aeronet/http-request-dispatch.hpp"

#include <cassert>
#include <exception>

#include "aeronet/http-constants.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/log.hpp"
#include "aeronet/middleware.hpp"
#include "aeronet/router.hpp"
#include "aeronet/tracing/tracer.hpp"

namespace aeronet {

namespace {

inline tracing::SpanRAII StartMiddlewareSpan(const HttpRequest& request, MiddlewareMetrics::Phase phase, uint32_t index,
                                             bool isGlobal, bool streaming, tracing::SpanPtr span) {
  tracing::SpanRAII spanScope(std::move(span));

  if (spanScope.span) {
    spanScope.span->setAttribute("aeronet.middleware.phase", phase == MiddlewareMetrics::Phase::Pre
                                                                 ? std::string_view("request")
                                                                 : std::string_view("response"));
    spanScope.span->setAttribute("aeronet.middleware.scope",
                                 isGlobal ? std::string_view("global") : std::string_view("route"));
    spanScope.span->setAttribute("aeronet.middleware.index", static_cast<int64_t>(index));
    spanScope.span->setAttribute("aeronet.middleware.streaming", static_cast<int64_t>(streaming));
    spanScope.span->setAttribute("http.method", http::MethodToStr(request.method()));
    spanScope.span->setAttribute("http.target", request.path());
  }

  return spanScope;
}

inline void CallMiddlewareMetricsCallback(const HttpRequest& request, MiddlewareMetrics::Phase phase, bool isGlobal,
                                          uint32_t index, bool shortCircuited, bool threw, bool streaming,
                                          const MiddlewareMetricsCallback& metricsCallback) {
  if (metricsCallback) {
    MiddlewareMetrics metrics;
    metrics.phase = phase;
    metrics.isGlobal = isGlobal;
    metrics.shortCircuited = shortCircuited;
    metrics.threw = threw;
    metrics.streaming = streaming;
    metrics.method = request.method();
    metrics.index = index;
    metrics.requestPath = request.path();
    metricsCallback(metrics);
  }
}

void BuildAllowHeader(http::MethodBmp methodBitmap, HttpResponse& response) {
  for (http::MethodIdx methodIdx = 0; methodIdx < http::kNbMethods; ++methodIdx) {
    if (!http::IsMethodIdxSet(methodBitmap, methodIdx)) {
      continue;
    }
    response.headerAppendValue(http::Allow, http::MethodIdxToStr(methodIdx), ",");
  }
}

}  // namespace

SpecialMethodResult ProcessSpecialMethods(const HttpRequest& request, Router& router, const SpecialMethodConfig& config,
                                          const CorsPolicy* pCorsPolicy, std::string_view requestData) {
  SpecialMethodResult result;

  if (request.method() == http::Method::OPTIONS) {
    if (request.path() == "*") {
      // OPTIONS * request (target="*") should return an Allow header listing supported methods.
      result.response = HttpResponse(http::StatusCodeOK);
      const http::MethodBmp allowed = router.allowedMethods("*");
      BuildAllowHeader(allowed, *result.response);
      result.action = SpecialMethodResult::Action::Handled;
      return result;
    }

    const auto routeMethods = router.allowedMethods(request.path());
    if (pCorsPolicy != nullptr) {
      auto preflight = pCorsPolicy->handlePreflight(request, routeMethods);
      switch (preflight.status) {
        case CorsPolicy::PreflightResult::Status::Allowed:
          result.response = std::move(preflight.response);
          result.action = SpecialMethodResult::Action::Handled;
          return result;
        case CorsPolicy::PreflightResult::Status::OriginDenied:
          result.response = HttpResponse(http::StatusCodeForbidden);
          result.response->body(http::ReasonForbidden);
          result.action = SpecialMethodResult::Action::Handled;
          return result;
        case CorsPolicy::PreflightResult::Status::MethodDenied:
          result.response = HttpResponse(http::StatusCodeMethodNotAllowed);
          result.response->body(http::ReasonMethodNotAllowed);
          BuildAllowHeader(routeMethods, *result.response);
          result.action = SpecialMethodResult::Action::Handled;
          return result;
        case CorsPolicy::PreflightResult::Status::HeadersDenied:
          result.response = HttpResponse(http::StatusCodeForbidden);
          result.response->body(http::ReasonForbidden);
          result.action = SpecialMethodResult::Action::Handled;
          return result;
        default:
          // Not a preflight, fall through to normal processing
          assert(preflight.status == CorsPolicy::PreflightResult::Status::NotPreflight);
          break;
      }
    }
  } else {
    assert(request.method() == http::Method::TRACE);

    // TRACE: echo the received request message as the body with Content-Type: message/http
    // Respect configured TracePolicy. Default: Disabled.
    bool allowTrace;
    switch (config.tracePolicy) {
      case HttpServerConfig::TraceMethodPolicy::EnabledPlainAndTLS:
        allowTrace = true;
        break;
      case HttpServerConfig::TraceMethodPolicy::EnabledPlainOnly:
        // If this request arrived over TLS, disallow TRACE
        allowTrace = !config.isTls;
        break;
      case HttpServerConfig::TraceMethodPolicy::Disabled:
        [[fallthrough]];
      default:
        allowTrace = false;
        break;
    }

    if (allowTrace && !requestData.empty()) {
      // Echo the raw request data
      result.response = HttpResponse(requestData, http::ContentTypeMessageHttp);
      result.action = SpecialMethodResult::Action::Handled;
      return result;
    }

    // TRACE disabled or no request data -> Method Not Allowed
    result.response = HttpResponse(http::StatusCodeMethodNotAllowed);
    result.response->body(http::ReasonMethodNotAllowed);
    result.action = SpecialMethodResult::Action::Handled;
  }
  return result;
}

std::optional<HttpResponse> RunRequestMiddleware(HttpRequest& request, std::span<const RequestMiddleware> global,
                                                 std::span<const RequestMiddleware> chain,
                                                 const tracing::TelemetryContext& telemetryContext, bool streaming,
                                                 const MiddlewareMetricsCallback& metricsCallback) {
  std::optional<HttpResponse> result;

  auto applyChain = [&, streaming](std::span<const RequestMiddleware> middlewareChain, bool isGlobal) {
    for (uint32_t hookIdx = 0; hookIdx < middlewareChain.size(); ++hookIdx) {
      auto spanScope = StartMiddlewareSpan(request, MiddlewareMetrics::Phase::Pre, hookIdx, isGlobal, streaming,
                                           telemetryContext.createSpan("aeronet.middleware"));
      bool threwEx = false;
      bool shortCircuited = false;
      try {
        MiddlewareResult decision = middlewareChain[hookIdx](request);
        shortCircuited = decision.shouldShortCircuit();
        if (shortCircuited) {
          result = std::move(decision).takeResponse();
          break;
        }
      } catch (const std::exception& ex) {
        threwEx = true;
        log::error("Exception in {} request middleware: {}", isGlobal ? "global" : "route", ex.what());
        result = HttpResponse(http::StatusCodeInternalServerError);
      } catch (...) {
        threwEx = true;
        log::error("Unknown exception in {} request middleware", isGlobal ? "global" : "route");
        result = HttpResponse(http::StatusCodeInternalServerError);
      }

      if (spanScope.span) {
        spanScope.span->setAttribute("aeronet.middleware.exception", int64_t{threwEx});
        spanScope.span->setAttribute("aeronet.middleware.short_circuit", int64_t{shortCircuited});
      }

      CallMiddlewareMetricsCallback(request, MiddlewareMetrics::Phase::Pre, isGlobal, hookIdx, shortCircuited, threwEx,
                                    streaming, metricsCallback);

      if (result.has_value()) {
        break;
      }
    }
  };

  applyChain(global, true);
  if (!result.has_value()) {
    applyChain(chain, false);
  }

  return result;
}

void ApplyResponseMiddleware(const HttpRequest& request, HttpResponse& response,
                             std::span<const ResponseMiddleware> chain,
                             std::span<const ResponseMiddleware> globalMiddleware,
                             const tracing::TelemetryContext& telemetryContext, bool streaming,
                             const MiddlewareMetricsCallback& metricsCallback) {
  auto runChain = [&request, &response, &telemetryContext, &metricsCallback, streaming](
                      std::span<const ResponseMiddleware> postMiddleware, bool isGlobal) {
    for (uint32_t hookIdx = 0; hookIdx < postMiddleware.size(); ++hookIdx) {
      auto spanScope = StartMiddlewareSpan(request, MiddlewareMetrics::Phase::Post, hookIdx, isGlobal, streaming,
                                           telemetryContext.createSpan("aeronet.middleware"));
      bool threwEx = false;
      try {
        postMiddleware[hookIdx](request, response);
      } catch (const std::exception& ex) {
        threwEx = true;
        log::error("Exception in {} response middleware: {}", isGlobal ? "global" : "route", ex.what());
      } catch (...) {
        threwEx = true;
        log::error("Unknown exception in {} response middleware", isGlobal ? "global" : "route");
      }

      if (spanScope.span) {
        spanScope.span->setAttribute("aeronet.middleware.exception", int64_t{threwEx});
        spanScope.span->setAttribute("aeronet.middleware.short_circuit", int64_t{0});
      }
      CallMiddlewareMetricsCallback(request, MiddlewareMetrics::Phase::Post, isGlobal, hookIdx, false, threwEx,
                                    streaming, metricsCallback);
    }
  };
  runChain(chain, false);
  runChain(globalMiddleware, true);
}

}  // namespace aeronet
