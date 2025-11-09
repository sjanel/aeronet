#pragma once

#include <cstdint>
#include <functional>
#include <utility>

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

  static MiddlewareResult Continue() noexcept { return MiddlewareResult(Decision::Continue); }

  static MiddlewareResult ShortCircuit(HttpResponse response) noexcept {
    return {Decision::ShortCircuit, std::move(response)};
  }

  [[nodiscard]] bool shouldContinue() const noexcept { return _decision == Decision::Continue; }

  [[nodiscard]] bool shouldShortCircuit() const noexcept { return _decision == Decision::ShortCircuit; }

  [[nodiscard]] HttpResponse&& takeResponse() && noexcept { return std::move(_response); }

 private:
  explicit MiddlewareResult(Decision decision) noexcept : _decision(decision) {}

  MiddlewareResult(Decision decision, HttpResponse response) noexcept
      : _decision(decision), _response(std::move(response)) {}

  Decision _decision{Decision::Continue};
  HttpResponse _response;
};

// Middleware invoked before the route handler executes. It may mutate the request and
// return a short-circuit response to skip subsequent middleware and the handler.
using RequestMiddleware = std::function<MiddlewareResult(HttpRequest&)>;

// Middleware invoked after the handler produces a response. It can amend headers/body.
using ResponseMiddleware = std::function<void(const HttpRequest&, HttpResponse&)>;

}  // namespace aeronet
