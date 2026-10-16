#pragma once

#include <cstddef>
#include <limits>
#include <string_view>

#include "aeronet/http-response.hpp"

namespace aeronet {

// Describes the body of the final response, given to ResponseBodySink::onHead() along with its head.
struct ResponseBodyInfo {
  // Value of size when the size of the body is not known in advance.
  static constexpr std::size_t kUnknownSize = std::numeric_limits<std::size_t>::max();

  // Whether the size of the body is known before it is received.
  [[nodiscard]] constexpr bool sizeKnown() const noexcept { return size != kUnknownSize; }

  // Media type of the body: its Content-Type value, application/octet-stream for a body without Content-Type, empty
  // when the head announces no body (HEAD request, 204, 304) or an empty one.
  std::string_view contentType;

  // Exact number of bytes onBody() will deliver: the Content-Length of a body delivered as received (no content
  // coding, or decompression disabled), the size of a body buffered before its delivery, 0 for an empty body.
  // kUnknownSize when the body is chunked, delimited by the closing of the connection, or decoded as it arrives (its
  // Content-Length then counts the coded bytes).
  std::size_t size{kUnknownSize};
};

// Receives the body of a response as it arrives, instead of having HttpClient buffer it whole (see the
// HttpClient::request() overloads taking a sink). Meant for large or unbounded downloads: the client only keeps one
// read worth of body in memory, whatever the response size, and HttpClientConfig::maxResponseBytes only bounds the
// response head.
//
// For one request, the sink sees exactly one response: the final one. The responses of followed redirects, and those
// retried for their status (RetryConfig::retryStatuses), are buffered and discarded as usual. Once onHead() was called,
// the request is never retried: an error after it leaves the sink with a partial body.
//
// The callbacks run synchronously on the thread of the request. They must not use the HttpClient making the request.
// An exception thrown by a callback propagates out of the request, whose connection is then closed.
class ResponseBodySink {
 public:
  virtual ~ResponseBodySink() = default;

  // Called once, before any body byte, with the status and headers of the final response (without body), and the
  // description of its body. Return false to stop the transfer: the request then fails with HttpClientErrc::aborted.
  virtual bool onHead(const HttpResponse& head, const ResponseBodyInfo& body) = 0;

  // Called with each consecutive piece of the body, de-chunked and decoded like a buffered body (Content-Encoding is
  // then absent from the head). Never called with an empty piece. The view is only valid during the call. Return false
  // to stop the transfer: the request then fails with HttpClientErrc::aborted.
  virtual bool onBody(std::string_view chunk) = 0;

 protected:
  ResponseBodySink() noexcept = default;
};

}  // namespace aeronet
