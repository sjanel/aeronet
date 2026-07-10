#pragma once

#include <stdexcept>
#include <string_view>
#include <utility>

#include "aeronet/client-connection.hpp"
#include "aeronet/http-message.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-status-code.hpp"

namespace aeronet {

// Describes an outbound HTTP request.
//
// Rather than maintaining a bespoke header/body container, ClientRequest reuses HttpMessage as the
// header + body store (its single-buffer layout, header insertion and Content-Type/Content-Length
// management are exactly what we need to assemble a request). Only the request-specific bits
// (method + target URL) are kept separately. The status-line bytes HttpMessage carries are unused
// here and never serialized.
//
// Note: Content-Type/Content-Length are managed via body(); do not pass them to headerAddLine()
// (HttpMessage rejects reserved headers). Connection framing is handled by HttpClient.
class ClientRequest {
 public:
  ClientRequest() = default;

  ClientRequest(http::Method method, std::string_view url) : _fields(url.size(), http::StatusCodeOK), _method(method) {
    this->url(url);
  }

  ClientRequest& method(http::Method method) & {
    _method = method;
    return *this;
  }
  ClientRequest&& method(http::Method method) && { return std::move(this->method(method)); }

  ClientRequest& url(std::string_view url) & {
    if (url.size() > HttpMessage::kMaxReasonLength) {
      throw std::invalid_argument("URL is too long for an HTTP request");
    }
    _fields.reason(url);
    return *this;
  }
  ClientRequest&& url(std::string_view url) && { return std::move(this->url(url)); }

  // Append a header (duplicates allowed). Do not use for Content-Type/Content-Length (use body()).
  ClientRequest& headerAddLine(std::string_view name, std::string_view value) & {
    _fields.headerAddLine(name, value);
    return *this;
  }
  ClientRequest&& headerAddLine(std::string_view name, std::string_view value) && {
    return std::move(this->headerAddLine(name, value));
  }

  // Set a header, replacing any existing same-named header.
  ClientRequest& header(std::string_view name, std::string_view value) & {
    _fields.header(name, value);
    return *this;
  }
  ClientRequest&& header(std::string_view name, std::string_view value) && {
    return std::move(this->header(name, value));
  }

  ClientRequest& body(std::string_view body) & {
    _fields.body(body);
    return *this;
  }
  ClientRequest&& body(std::string_view body) && { return std::move(this->body(body)); }

  // Body with an explicit Content-Type in one call.
  ClientRequest& body(std::string_view body, std::string_view contentType) & {
    _fields.body(body, contentType);
    return *this;
  }
  ClientRequest&& body(std::string_view body, std::string_view contentType) && {
    return std::move(this->body(body, contentType));
  }

  [[nodiscard]] http::Method method() const noexcept { return _method; }

  // Get the URL (target) for this request.
  [[nodiscard]] std::string_view url() const noexcept { return _fields.reason(); }

  // Get a contiguous view of the current headers stored in this request.
  // Each header line is formatted as: name + ": " + value + CRLF. If no headers are present, it
  // returns an empty view.
  [[nodiscard]] std::string_view headersFlatView() const noexcept { return _fields.headersFlatView(); }

  // Return a non-allocating, iterable view over headers.
  // Each element is a HeaderView with name and value string_views.
  // Usage example:
  //   for (const auto &[name, value] : request.headers()) {
  //       process(name, value);
  //   }
  [[nodiscard]] HeadersView headers() const noexcept { return _fields.headers(); }

  [[nodiscard]] std::string_view body() const noexcept { return _fields.bodyInMemory(); }

 private:
  friend class HttpClient;

  HttpMessage _fields;  // container used to store url (reason), header + body
  http::Method _method{http::Method::GET};
};

}  // namespace aeronet
