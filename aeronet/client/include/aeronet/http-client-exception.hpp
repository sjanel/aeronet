#pragma once

#include <stdexcept>
#include <string>

namespace aeronet {

// Thrown only for hard *setup* errors detected when building the client or its TLS context: codec or
// certificate/key misconfiguration, an unusable cipher list, etc. (https requested in a build without
// OpenSSL surfaces as std::logic_error instead).
//
// Per-request *runtime* failures (invalid URL, DNS/connect failure, timeout, TLS handshake error,
// malformed/oversized response, ...) are NOT thrown: they are returned as an HttpClientErrc in the
// HttpClientResult error state. A non-2xx HTTP status is likewise not an error -- it is a normal
// HttpResponse in the success state.
class HttpClientException : public std::runtime_error {
 public:
  explicit HttpClientException(const std::string& what) : std::runtime_error(what) {}
  explicit HttpClientException(const char* what) : std::runtime_error(what) {}
};

}  // namespace aeronet
