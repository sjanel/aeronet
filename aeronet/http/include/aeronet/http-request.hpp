#pragma once

#include <cstddef>
#include <string_view>

#include "connection-state.hpp"
#include "headers-view-map.hpp"
#include "http-method.hpp"
#include "http-status-code.hpp"
#include "http-version.hpp"

namespace aeronet {

class HttpRequest {
 public:
  // Get the HTTP header corresponding to given key.
  // Search is case insensitive.
  // Return empty view if not present.
  [[nodiscard]] std::string_view header(std::string_view key) const;

  // The method of the request (GET, PUT, ...)
  [[nodiscard]] http::Method method() const { return _method; }

  // The URL decoded path.
  // Example:
  //  GET /path         -> '/path'
  //  GET /path?key=val -> '/path'
  [[nodiscard]] std::string_view path() const { return _path; }

  // Get the HTTP version of the request.
  [[nodiscard]] http::Version version() const { return _version; }

  // Get a reference to a map like object of the HTTP headers attached to this request.
  [[nodiscard]] const auto& headers() const { return _headers; }

  // =============================
  // Lazy query parameter iteration
  // =============================
  // Provides zero-allocation iteration over key/value pairs in the raw query string.
  // Decoding rules (application/x-www-form-urlencoded semantics for each component ONLY):
  //  - Percent escapes decoded independently for key & value; malformed/incomplete escapes left verbatim.
  //  - '+' translated to space (' ') in keys and values.
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
      using value_type = QueryParam;
      using difference_type = std::ptrdiff_t;
      using reference = value_type;  // returned by value (small POD)
      using pointer = void;          // not providing pointer semantics

      iterator() noexcept = default;

      iterator(std::string_view full, std::size_t pos) : _full(full), _pos(pos) { advance(); }

      value_type operator*() const { return _current; }

      iterator& operator++() {
        advance();
        return *this;
      }

      bool operator==(const iterator& other) const { return _pos == other._pos && _full.data() == other._full.data(); }

     private:
      void advance();

      std::string_view _full;  // underlying full query
      std::size_t _pos{std::string_view::npos};
      QueryParam _current{};
      bool _atEnd{false};
    };

    [[nodiscard]] iterator begin() const { return {_fullQuery, 0}; }

    [[nodiscard]] iterator end() const { return {_fullQuery, std::string_view::npos}; }

   private:
    friend class HttpRequest;

    explicit QueryParamRange(std::string_view fullQuery) : _fullQuery(fullQuery) {}

    std::string_view _fullQuery;
  };

  // Get an iterable range on URL decoded query params.
  // The duplicates are preserved.
  // This function is non-allocating.
  // Example:
  //    for (const auto &[queryParamKey, queryParamValue] : httpRequest.queryParams()) {
  //       // do something with queryParamKey and queryParamValue
  //    }
  [[nodiscard]] QueryParamRange queryParams() const { return QueryParamRange(_decodedQueryParams); }

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
  friend class HttpRequestUnit_ParseBasicPathAndVersion_Test;  // gtest generated
  friend class HttpRequestUnit_QueryParamsDecodingPlusAndPercent_Test;
  friend class HttpRequestUnit_EmptyAndMissingValues_Test;
  friend class HttpRequestUnit_DuplicateKeysPreservedOrder_Test;
  friend class HttpRequestUnit_InvalidPathEscapeCauses400_Test;

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