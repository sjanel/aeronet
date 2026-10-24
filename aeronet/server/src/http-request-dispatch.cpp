#include "aeronet/http-request-dispatch.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <span>
#include <string_view>
#include <utility>

#include "aeronet/cors-policy.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request-view.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/https-redirect.hpp"
#include "aeronet/internal/http-response-access.hpp"
#include "aeronet/log.hpp"
#include "aeronet/middleware.hpp"
#include "aeronet/path-handlers.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/router.hpp"
#include "aeronet/tracing/tracer.hpp"

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
#include "aeronet/request-task.hpp"
#endif

namespace aeronet {

namespace {

tracing::SpanRAII StartMiddlewareSpan(const HttpRequestView& request, MiddlewareMetrics::Phase phase, uint32_t index,
                                      bool isGlobal, bool streaming, tracing::SpanPtr span) {
  tracing::SpanRAII spanScope(std::move(span));

  if (spanScope.span) {
    spanScope.span->setAttribute("aeronet.middleware.phase",
                                 phase == MiddlewareMetrics::Phase::Pre ? "request" : "response");
    spanScope.span->setAttribute("aeronet.middleware.scope", isGlobal ? "global" : "route");
    spanScope.span->setAttribute("aeronet.middleware.index", static_cast<int64_t>(index));
    spanScope.span->setAttribute("aeronet.middleware.streaming", static_cast<int64_t>(streaming));
    spanScope.span->setAttribute("http.method", http::MethodToStr(request.method()));
    spanScope.span->setAttribute("http.target", request.path());
  }

  return spanScope;
}

void CallMiddlewareMetricsCallback(const HttpRequestView& request, MiddlewareMetrics::Phase phase, bool isGlobal,
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

HttpResponse ProcessSpecialMethods(const HttpRequestView& request, Router& router, SpecialMethodConfig config,
                                   const CorsPolicy* pCorsPolicy, std::string_view requestData) {
  HttpResponse result = internal::HttpResponseAccess::MakeUnallocated();

  if (request.method() == http::Method::OPTIONS) {
    if (request.path() == "*") {
      // OPTIONS * request (target="*") should return an Allow header listing supported methods.
      result = request.makeResponse(http::StatusCodeOK);
      const http::MethodBmp allowed = router.allowedMethods("*");
      BuildAllowHeader(allowed, result);
      return result;
    }

    const auto routeMethods = router.allowedMethods(request.path());
    if (pCorsPolicy != nullptr) {
      auto preflight = pCorsPolicy->handlePreflight(request, routeMethods);
      switch (preflight.status) {
        case CorsPolicy::PreflightResult::Status::Allowed:
          result = std::move(preflight.response);
          return result;
        case CorsPolicy::PreflightResult::Status::HeadersDenied:
          [[fallthrough]];
        case CorsPolicy::PreflightResult::Status::OriginDenied:
          result = request.makeResponse(http::StatusCodeForbidden);
          result.body(http::ReasonForbidden);
          return result;
        case CorsPolicy::PreflightResult::Status::MethodDenied:
          result = request.makeResponse(http::StatusCodeMethodNotAllowed);
          result.body(http::ReasonMethodNotAllowed);
          BuildAllowHeader(routeMethods, result);
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
      default:
        assert(config.tracePolicy == HttpServerConfig::TraceMethodPolicy::Disabled);
        allowTrace = false;
        break;
    }

    if (allowTrace && !requestData.empty()) {
      // Echo the raw request data
      result = request.makeResponse(requestData, http::ContentTypeMessageHttp);
      return result;
    }

    // TRACE disabled or no request data -> Method Not Allowed
    result = request.makeResponse(http::StatusCodeMethodNotAllowed);
    result.body(http::ReasonMethodNotAllowed);
  }
  return result;
}

HttpResponse RequestPipeline::runRequestMiddleware(HttpRequestView& request, const Router::RoutingResult& routingResult,
                                                   bool streaming) const {
  request.finalizeBeforeHandlerCall(routingResult.pathParams());

  HttpResponse result = internal::HttpResponseAccess::MakeUnallocated();

  auto applyChain = [&, streaming](std::span<const RequestMiddleware> middlewareChain, bool isGlobal) {
    for (uint32_t hookIdx = 0; hookIdx < middlewareChain.size(); ++hookIdx) {
      auto spanScope = StartMiddlewareSpan(request, MiddlewareMetrics::Phase::Pre, hookIdx, isGlobal, streaming,
                                           _pTelemetry->createSpan("aeronet.middleware"));
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
        result = request.makeResponse(http::StatusCodeInternalServerError);
      } catch (...) {
        threwEx = true;
        log::error("Unknown exception in {} request middleware", isGlobal ? "global" : "route");
        result = request.makeResponse(http::StatusCodeInternalServerError);
      }

      if (spanScope.span) {
        spanScope.span->setAttribute("aeronet.middleware.exception", int64_t{threwEx});
        spanScope.span->setAttribute("aeronet.middleware.short_circuit", int64_t{shortCircuited});
      }

      CallMiddlewareMetricsCallback(request, MiddlewareMetrics::Phase::Pre, isGlobal, hookIdx, shortCircuited, threwEx,
                                    streaming, *_pMiddlewareMetrics);

      if (!result.empty()) {
        break;
      }
    }
  };

  applyChain(_pRouter->globalRequestMiddleware(), true);
  if (result.empty()) {
    applyChain(routingResult.preMiddlewareRange(), false);
  }

  return result;
}

HttpResponse RequestPipeline::callRequestHandler(HttpRequestView& request, const Router::RoutingResult& routingResult,
                                                 bool streaming) const {
  const Router::RoutingResult::HandlerKind handlerKind = routingResult.handlerKind();
  // The streaming handler of a request that cannot be streamed (HTTP/1.0) is not called: answered like without handler.
  if (handlerKind != Router::RoutingResult::HandlerKind::None &&
      (handlerKind != Router::RoutingResult::HandlerKind::Streaming || streaming)) {
    if (IsOriginDenied(request, routingResult)) {
      return MakeOriginDenial(request);
    }
    const RequestHandler* pHandler = routingResult.requestHandler();
    if (pHandler == nullptr) {
      return internal::HttpResponseAccess::MakeUnallocated();
    }
    try {
      return (*pHandler)(request);
    } catch (const std::exception& ex) {
      log::error("Exception in path handler: {}", ex.what());
    } catch (...) {
      log::error("Unknown exception in path handler");
    }
    return request.makeResponse(http::StatusCodeInternalServerError);
  }
  if (routingResult.redirectSlashMode() != Router::RoutingResult::RedirectSlashMode::None) {
    return makeTrailingSlashRedirect(request, routingResult.redirectSlashMode());
  }
  if (routingResult.methodNotAllowed()) {
    return request.makeResponse(http::StatusCodeMethodNotAllowed, http::ReasonMethodNotAllowed);
  }
  return request.makeResponse(http::StatusCodeNotFound);
}

HttpResponse RequestPipeline::MakeOriginDenial(const HttpRequestView& request) {
  return request.makeResponse(http::StatusCodeForbidden, "Forbidden by CORS policy");
}

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
RequestTask<HttpResponse> RequestPipeline::CallAsyncHandler(const AsyncRequestHandler& handler,
                                                            HttpRequestView& request) {
  RequestTask<HttpResponse> task;
  try {
    task = handler(request);
  } catch (const std::exception& ex) {
    log::error("Exception in async path handler for path {}: {}", request.path(), ex.what());
    return task;
  } catch (...) {
    log::error("Unknown exception in async path handler for path {}", request.path());
    return task;
  }
  if (!task.valid()) {
    log::error("Async path handler returned an invalid RequestTask for path {}", request.path());
  }
  return task;
}
#endif

void RequestPipeline::applyResponseMiddleware(const HttpRequestView& request, HttpResponse& response,
                                              std::span<const ResponseMiddleware> routeChain, bool streaming) const {
  auto runChain = [this, &request, &response, streaming](std::span<const ResponseMiddleware> postMiddleware,
                                                         bool isGlobal) {
    for (uint32_t hookIdx = 0; hookIdx < postMiddleware.size(); ++hookIdx) {
      auto spanScope = StartMiddlewareSpan(request, MiddlewareMetrics::Phase::Post, hookIdx, isGlobal, streaming,
                                           _pTelemetry->createSpan("aeronet.middleware"));
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
                                    streaming, *_pMiddlewareMetrics);
    }
  };
  runChain(routeChain, false);
  runChain(_pRouter->globalResponseMiddleware(), true);
}

void RequestPipeline::prepareStreamingHead(const HttpRequestView& request, HttpResponse& head,
                                           std::span<const ResponseMiddleware> routeChain,
                                           const CorsPolicy* pCorsPolicy) const {
  static constexpr bool kStreaming = true;
  applyResponseMiddleware(request, head, routeChain, kStreaming);
  if (pCorsPolicy != nullptr) {
    (void)pCorsPolicy->applyToResponse(request, head);
  }
}

HttpResponse RequestPipeline::makeTrailingSlashRedirect(
    const HttpRequestView& request, Router::RoutingResult::RedirectSlashMode redirectSlashMode) const {
  // The path is percent-decoded: re-encode it so that the Location is a valid URI reference. Decoded control
  // characters (CR, LF...) would otherwise make the header value invalid and throw outside of any handler.
  static constexpr std::string_view kRedirecting = "Redirecting";
  const std::string_view reqPath = request.path();
  RawChars& location = *_pScratch;
  location.clear();
  if (redirectSlashMode == Router::RoutingResult::RedirectSlashMode::AddSlash) {
    http::AppendUrlEncodedPath(location, reqPath);
    location.push_back('/');
  } else {
    http::AppendUrlEncodedPath(location, reqPath.substr(0, reqPath.size() - 1));
  }
  const std::size_t additionalCapacity =
      HttpResponse::BodySize(kRedirecting.size()) + http::HeaderSize(http::Location.size(), location.size());
  auto resp = request.makeResponse(additionalCapacity, http::StatusCodeMovedPermanently);
  resp.headerAddLine(http::Location, std::string_view(location));
  resp.body(kRedirecting);
  return resp;
}

}  // namespace aeronet
