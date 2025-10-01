#pragma once

#include <cstddef>
#include <optional>
#include <string_view>

#include "aeronet/http-method.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/http-version.hpp"
#include "connection-state.hpp"
#include "headers-view-map.hpp"

namespace aeronet {

class HttpRequest {
 public:
  // Get the HTTP header value for the given key.
  // - Lookup is case-insensitive (RFC 7230 token rules; simple lower/compare internally).
  // - Returns empty string_view if the header is absent.
  // - Leading and trailing whitespace (spaces / tabs) around the value are trimmed.
  [[nodiscard]] std::string_view headerValueOrEmpty(std::string_view headerKey) const noexcept;

  // Get the HTTP header value for the given key, if present.
  // - Returns std::nullopt when the header is absent.
  // - Returns an engaged optional containing an empty string_view when the header exists with an empty value.
  // - Leading and trailing whitespace are trimmed; internal whitespace is preserved verbatim.
  // This removes ambiguity between a missing header and a present header whose value is the empty string.
  [[nodiscard]] std::optional<std::string_view> headerValue(std::string_view headerKey) const noexcept;

  // The method of the request (GET, PUT, ...)
  [[nodiscard]] http::Method method() const noexcept { return _method; }

  // The URL decoded path.
  // Example:
  //  GET /path         -> '/path'
  //  GET /path?key=val -> '/path'
  [[nodiscard]] std::string_view path() const noexcept { return _path; }

  // Get the HTTP version of the request.
  [[nodiscard]] http::Version version() const noexcept { return _version; }

  // Get a reference to a map like object of the HTTP headers attached to this request.
  // The header values will be returned without trailing spaces.
  // Headers with empty values will be returned.
  [[nodiscard]] const auto& headers() const noexcept { return _headers; }

  // =============================
  // Lazy query parameter iteration
  // =============================
  // Provides zero-allocation iteration over key/value pairs in the raw query string.
  // Decoding rules (application/x-www-form-urlencoded semantics for each component ONLY):
  //  - Percent escapes decoded independently for key & value; malformed/incomplete escapes left verbatim.
  //  - '+' translated to space (' ') in values.
  //  - Missing '=' => value = "". Empty key allowed ("=val" -> key="", value="val").
  //  - Duplicate keys preserved in order.
  struct QueryParam {
    std::string_view key;
    std::string_view value;
  };
  class QueryParamRange {
   public:
    class iterator {
     public:
      iterator(const char* begKey, const char* endFullQuery) : _begKey(begKey), _endFullQuery(endFullQuery) {}

      QueryParam operator*() const;

      iterator& operator++() {
        advance();
        return *this;
      }

      bool operator==(const iterator& other) const { return _begKey == other._begKey; }

     private:
      void advance();

      const char* _begKey;
      const char* _endFullQuery;
    };

    [[nodiscard]] iterator begin() const noexcept { return {_fullQuery.data(), _fullQuery.data() + _fullQuery.size()}; }

    [[nodiscard]] iterator end() const noexcept {
      return {_fullQuery.data() + _fullQuery.size(), _fullQuery.data() + _fullQuery.size()};
    }

   private:
    friend class HttpRequest;

    explicit QueryParamRange(std::string_view fullQuery) noexcept : _fullQuery(fullQuery) {}

    std::string_view _fullQuery;
  };

  // Get an iterable range on URL decoded query params.
  // The duplicates are preserved.
  // This function is non-allocating.
  // Example:
  //    for (const auto &[queryParamKey, queryParamValue] : httpRequest.queryParams()) {
  //       // do something with queryParamKey and queryParamValue
  //    }
  [[nodiscard]] QueryParamRange queryParams() const noexcept { return QueryParamRange(_decodedQueryParams); }

  // Get the (already received) body of the request.
  [[nodiscard]] std::string_view body() const noexcept { return _body; }

  // Selected ALPN protocol (if negotiated); empty if none or not TLS.
  [[nodiscard]] std::string_view alpnProtocol() const noexcept { return _alpnProtocol; }

  // Negotiated TLS cipher suite; empty if connection not using TLS.
  [[nodiscard]] std::string_view tlsCipher() const noexcept { return _tlsCipher; }

  // Negotiated TLS protocol version string (e.g. "TLSv1.3"); empty if not TLS.
  [[nodiscard]] std::string_view tlsVersion() const noexcept { return _tlsVersion; }

 private:
  friend class HttpServer;
  friend class HttpRequestTest;

  [[nodiscard]] bool wantClose() const;

  // Attempts to set this HttpRequest (except body) from given ConnectionState.
  // Returns StatusCode OK if the request is good (it will be fully set)
  // Or an HTTP error status to forward.
  http::StatusCode setHead(ConnectionState& state, std::size_t maxHeadersBytes);

  http::Version _version;
  http::Method _method;
  std::string_view _path;
  std::string_view _flatHeaders;

  HeadersViewMap _headers;

  // Raw query component (excluding '?') retained as-is; per-key/value decoding happens on iteration.
  // Lifetime: valid only during handler invocation (points into connection buffer / in-place decode area).
  std::string_view _decodedQueryParams;
  std::string_view _body;
  std::string_view _alpnProtocol;  // Selected ALPN protocol (if any) for this connection, empty if none/unsupported
  std::string_view _tlsCipher;     // Negotiated cipher suite (empty if not TLS)
  std::string_view _tlsVersion;    // Negotiated TLS protocol version (e.g. TLSv1.3) empty if not TLS
};

}  // namespace aeronet