#pragma once

#include <cstdint>
#include <functional>
#include <string_view>

#include "aeronet/http-response.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/request-metrics.hpp"

namespace aeronet {

class HttpRequestView;

// Callback invoked when the request parser encounters a non-recoverable protocol error (see
// SingleHttpServer::setParserErrorCallback()).
using ParserErrorCallback = std::function<void(http::StatusCode)>;

// Callback invoked after completing each request (see SingleHttpServer::setMetricsCallback()).
using MetricsCallback = std::function<void(const RequestMetrics&)>;

// Expectation handling API
// ------------------------
// The server will honour the standard "Expect: 100-continue" behaviour by default.
// For other Expect tokens, applications may register an ExpectationHandler to
// implement custom semantics (for example sending an interim 102 Processing
// or producing a final response). If no handler is registered and the Expect
// header contains any token other than "100-continue", the server will respond
// with 417 Expectation Failed per RFC and reject the request.
enum class ExpectationResultKind : uint8_t { Continue, Interim, FinalResponse, Reject };

struct ExpectationResult {
  ExpectationResultKind kind = ExpectationResultKind::Continue;
  // Used for Interim when the handler wants the server to emit an interim
  // response with the given status code (e.g. 102). Stored as uint8_t because
  // only the 1xx class (100-199) is valid and fits in one byte. 101 is refused
  // (answered 500): Switching Protocols is not an interim response.
  uint8_t interimStatus = 0;
  // Used for FinalResponse when the handler wishes to reply immediately
  // with a full HttpResponse (the server will send it and skip reading the body).
  HttpResponse finalResponse;
};

// Handler of the Expect tokens other than "100-continue" (see SingleHttpServer::setExpectationHandler()).
using ExpectationHandler = std::function<ExpectationResult(const HttpRequestView&, std::string_view)>;

}  // namespace aeronet
