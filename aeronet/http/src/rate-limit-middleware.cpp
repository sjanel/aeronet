#include "aeronet/rate-limit-middleware.hpp"

#include <cassert>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/middleware.hpp"
#include "aeronet/ndigits.hpp"
#include "aeronet/rate-limit.hpp"
#include "aeronet/string-trim.hpp"

namespace aeronet {
namespace {

std::string_view ResolveKey(const HttpRequest& request, const RateLimitRequestMiddlewareBuilder& options) {
  switch (options.keyStrategy) {
    case RateLimitClientKeyStrategy::PeerAddress:
      return request.clientAddress();
    case RateLimitClientKeyStrategy::XForwardedForFirst: {
      auto xff = request.headerValueOrEmpty("x-forwarded-for");
      if (xff.empty()) {
        return {};
      }
      const auto comma = xff.find(',');
      xff = xff.substr(0, comma);
      return TrimOws(xff);
    }
    case RateLimitClientKeyStrategy::HeaderValue:
      return TrimOws(request.headerValueOrEmpty(options.headerName));
    default:
      assert(options.keyStrategy == RateLimitClientKeyStrategy::Custom);
      if (!options.customKeyExtractor) {
        return {};
      }
      return options.customKeyExtractor(request);
  }
}

RequestMiddleware BuildRateLimitMiddleware(RateLimitRequestMiddlewareBuilder options) {
  options.config.validate();

  if (!options.store) {
    options.store = std::make_shared<InMemoryTokenBucketRateLimitStore>();
  }

  if (options.keyStrategy == RateLimitClientKeyStrategy::HeaderValue && options.headerName.empty()) {
    throw std::invalid_argument("RateLimitRequestMiddlewareBuilder.headerName must be set for HeaderValue strategy");
  }

  return [opts = std::move(options)](HttpRequest& request) mutable {
    const std::string_view key = ResolveKey(request, opts);
    if (key.empty()) {
      return MiddlewareResult::Continue();
    }

    RateLimitDecision decision;
    try {
      decision = opts.store->consume(key, std::chrono::steady_clock::now(), opts.config);
    } catch (...) {
      if (opts.config.failOpen) {
        return MiddlewareResult::Continue();
      }
      decision = RateLimitDecision::Reject(1);
    }

    if (decision.allowed) {
      return MiddlewareResult::Continue();
    }

    static constexpr std::string_view kRetryAfterHeader = "Retry-After";

    auto response =
        request.makeResponse(HttpResponse::HeaderSize(kRetryAfterHeader.size(), ndigits(decision.retryAfterSeconds)) +
                                 HttpResponse::BodySize(opts.rejectionBody.size()),
                             http::StatusCodeTooManyRequests);

    response.headerAddLine("Retry-After", decision.retryAfterSeconds);
    response.body(opts.rejectionBody);
    return MiddlewareResult::ShortCircuit(std::move(response));
  };
}
}  // namespace

RequestMiddleware RateLimitRequestMiddlewareBuilder::build() const& { return BuildRateLimitMiddleware(*this); }

RequestMiddleware RateLimitRequestMiddlewareBuilder::build() && { return BuildRateLimitMiddleware(std::move(*this)); }

}  // namespace aeronet
