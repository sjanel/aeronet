#pragma once

#include <cstdint>
#include <functional>
#include <string_view>
#include <utility>

#include "aeronet/http-method.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"

namespace aeronet {

// Result of running a middleware stage.
class MiddlewareResult {
 public:
  enum class Decision : std::uint8_t { Continue, ShortCircuit };

  // Default to Continue.
  // Synonym of MiddlewareResult::Continue.
  MiddlewareResult() noexcept = default;

  // Constructor to short-circuit response with given one.
  // Synonym of MiddlewareResult::ShortCircuit.
  explicit MiddlewareResult(HttpResponse response) noexcept
      : _decision(Decision::ShortCircuit), _response(std::move(response)) {}

  // Returns a MiddlewareResult indicating to continue processing.
  static MiddlewareResult Continue() noexcept { return {}; }

  // Returns a MiddlewareResult indicating to short-circuit with the given response.
  static MiddlewareResult ShortCircuit(HttpResponse response) noexcept { return MiddlewareResult{std::move(response)}; }

  [[nodiscard]] bool shouldContinue() const noexcept { return _decision == Decision::Continue; }

  [[nodiscard]] bool shouldShortCircuit() const noexcept { return _decision == Decision::ShortCircuit; }

  [[nodiscard]] HttpResponse&& takeResponse() && noexcept { return std::move(_response); }

 private:
  Decision _decision{Decision::Continue};
  HttpResponse _response;
};

struct MiddlewareMetrics {
  enum class Phase : uint8_t { Pre, Post };

  Phase phase;
  bool isGlobal;
  bool shortCircuited;
  bool threw;
  bool streaming;
  http::Method method;
  uint32_t index;
  std::string_view requestPath;
};

using MiddlewareMetricsCallback = std::function<void(const MiddlewareMetrics&)>;

// Middleware invoked before the route handler executes. It may mutate the request and
// return a short-circuit response to skip subsequent middleware and the handler.
using RequestMiddleware = std::function<MiddlewareResult(HttpRequest&)>;

// Middleware invoked after the handler produces a response. It can amend headers/body.
using ResponseMiddleware = std::function<void(const HttpRequest&, HttpResponse&)>;

}  // namespace aeronet
