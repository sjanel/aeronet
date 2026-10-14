#pragma once

#include <string_view>

#include "aeronet/http-response.hpp"

namespace aeronet {

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

  // Called once, before any body byte, with the status and headers of the final response (without body), and the media
  // type of its body: its Content-Type value, application/octet-stream for a body without Content-Type, empty when
  // the head announces no body (HEAD request, 204, 304). Return false to stop the transfer: the request then fails with
  // HttpClientErrc::aborted.
  virtual bool onHead(const HttpResponse& head, std::string_view contentType) = 0;

  // Called with each consecutive piece of the body, de-chunked and decoded like a buffered body (Content-Encoding is
  // then absent from the head). Never called with an empty piece. The view is only valid during the call. Return false
  // to stop the transfer: the request then fails with HttpClientErrc::aborted.
  virtual bool onBody(std::string_view chunk) = 0;

 protected:
  ResponseBodySink() noexcept = default;
};

}  // namespace aeronet
