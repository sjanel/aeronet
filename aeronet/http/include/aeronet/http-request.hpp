#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <string_view>

#include "aeronet/connection-state.hpp"
#include "aeronet/headers-view-map.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/http-version.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/tracing/tracer.hpp"

namespace aeronet {

class HttpRequest {
 public:
  // Returns the (possibly merged) HTTP header value for the given key or an empty string_view if absent.
  // Semantics / behavior:
  //   * Lookup is case-insensitive (RFC 7230 token rules).
  //   * Duplicate request headers are canonicalized in-place during parsing according to a constexpr
  //     classification table (see README section "Request Header Duplicate Handling"):
  //       - List-style headers (e.g. Accept, Via, Warning) are comma-joined:  "v1,v2"
  //       - Cookie is semicolon-joined:                                       "c1;c2" (no added space)
  //       - User-Agent tokens are space-joined:                               "Foo Bar"
  //       - Override headers (Authorization, Range, From, select conditionals) keep ONLY the last occurrence.
  //       - Disallowed duplicates (Host, Content-Length) trigger 400 before a value is returned here.
  //     Unknown headers currently default to list (comma) merge.
  //   * Empty value handling avoids manufacturing leading/trailing separators:
  //       first="" + second="v"  -> "v"
  //       first="v" + second=""  -> "v" (unchanged)
  //   * Leading & trailing horizontal whitespaces around the original field value are trimmed; internal whitespaces
  //     are preserved verbatim (except for deliberate single-space joins in the User-Agent merge case).
  //   * The returned view points into the connection's receive buffer; it is valid only for the lifetime of the
  //     handler invocation (do not persist it beyond the request scope).
  //   * If you need to distinguish between a missing header and an explicitly present empty header, use headerValue().
  [[nodiscard]] std::string_view headerValueOrEmpty(std::string_view headerKey) const noexcept;

  // Like headerValueOrEmpty() but preserves the distinction between absence and an explicitly empty value.
  //   * std::nullopt  => header not present in the request.
  //   * engaged empty => header present with zero-length (after trimming) value.
  //   * engaged non-empty => possibly merged / override-normalized value (see duplicate handling above).
  // All trimming, merge, override, and lifetime notes from headerValueOrEmpty() apply here.
  // Use this when protocol logic must differentiate between omitted vs intentionally blank headers.
  [[nodiscard]] std::optional<std::string_view> headerValue(std::string_view headerKey) const noexcept;

  // The method of the request (GET, PUT, ...)
  [[nodiscard]] http::Method method() const noexcept { return _method; }

  // The URL decoded path (the target without the query params string).
  // Example:
  //  GET /path               -> '/path'
  //  GET /path?key=val       -> '/path'
  //  GET /path%2Caaa?key=val -> '/path,aaa'
  [[nodiscard]] std::string_view path() const noexcept { return _path; }

  // Get the HTTP version of the request.
  [[nodiscard]] http::Version version() const noexcept { return _version; }

  // Returns a map-like, case-insensitive view over the parsed request headers.
  // Characteristics:
  //   * Value semantics reflect the in-place duplicate handling policy (merged / overridden as documented above so
  //     there will be at most one entry per header name).
  //   * Iteration order is not necessarily the same as the original HTTP request
  //   * Values are string_view slices into the connection buffer; valid only during the handler call.
  //   * Trailing & leading horizontal whitespace around each original field-value are removed.
  //   * Empty headers are retained (key maps to empty string_view) allowing explicit empties to be detected via
  //     headerValue().
  [[nodiscard]] const auto& headers() const noexcept { return _headers; }

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
  // The order of entries and duplicates are preserved.
  // This function is non-allocating.
  // Example:
  //    for (const auto &[queryParamKey, queryParamValue] : httpRequest.queryParams()) {
  //       // do something with queryParamKey and queryParamValue
  //    }
  [[nodiscard]] QueryParamRange queryParams() const noexcept { return QueryParamRange(_decodedQueryParams); }

  // Get the (already received) body of the request.
  [[nodiscard]] std::string_view body() const noexcept { return _body; }

  // Returns a map-like, case-insensitive view over trailer headers received after a chunked body (RFC 7230 §4.1.2).
  // Characteristics:
  //   * Only populated for chunked requests; empty for fixed Content-Length or bodyless requests.
  //   * Same duplicate-header merge policy as regular headers (comma-join, override, etc.).
  //   * Values are string_view slices into the connection buffer; valid only during the handler call.
  //   * Forbidden trailer fields (Transfer-Encoding, Content-Length, Host, etc.) are rejected with 400.
  //   * Trailers count toward the maxHeadersBytes limit (combined with initial headers).
  [[nodiscard]] const auto& trailers() const noexcept { return _trailers; }

  // Returns a map-like view over path parameters extracted during route matching.
  // Characteristics:
  //   * Key/value views point into the connection buffer; valid only during the handler call.
  //   * Values are already percent-decoded.
  //   * The order of entries is not specified.
  //   * If the patterns were unnamed, the keys are numeric strings representing the 0-based index of the match.
  [[nodiscard]] const auto& pathParams() const noexcept { return _pathParams; }

  // Selected ALPN protocol (if negotiated); empty if none or not TLS.
  [[nodiscard]] std::string_view alpnProtocol() const noexcept { return _alpnProtocol; }

  // Negotiated TLS cipher suite; empty if connection not using TLS.
  [[nodiscard]] std::string_view tlsCipher() const noexcept { return _tlsCipher; }

  // Negotiated TLS protocol version string (e.g. "TLSv1.3"); empty if not TLS.
  [[nodiscard]] std::string_view tlsVersion() const noexcept { return _tlsVersion; }

  // Tells whether this request has a 'Expect: 100-continue' header.
  [[nodiscard]] bool hasExpectContinue() const noexcept;

  // Timestamp when request parsing began.
  [[nodiscard]] std::chrono::steady_clock::time_point reqStart() const noexcept { return _reqStart; }

 private:
  friend class HttpServer;
  friend class HttpRequestTest;
  friend class StaticFileHandlerTest;
  friend class CorsPolicyHarness;

  HttpRequest() noexcept = default;

  [[nodiscard]] bool wantClose() const;

  // Attempts to set this HttpRequest (except body) from given ConnectionState.
  // Returns StatusCode OK if the request is good (it will be fully set) or an HTTP error status to forward.
  // If 0 is returned, it means the connection state buffer is not filled up to the first newline.
  http::StatusCode initTrySetHead(ConnectionState& state, RawChars& tmpBuffer, std::size_t maxHeadersBytes,
                                  bool mergeAllowedForUnknownRequestHeaders, tracing::SpanPtr traceSpan);

  void shrink_to_fit();

  void end(http::StatusCode respStatusCode);

  http::Version _version;
  http::Method _method;
  std::string_view _path;
  std::string_view _flatHeaders;

  HeadersViewMap _headers;
  HeadersViewMap _trailers;  // Trailer headers (RFC 7230 §4.1.2) from chunked requests
  flat_hash_map<std::string_view, std::string_view> _pathParams;

  // Raw query component (excluding '?') retained as-is; per-key/value decoding happens on iteration.
  // Lifetime: valid only during handler invocation (points into connection buffer / in-place decode area).
  std::string_view _decodedQueryParams;
  std::string_view _body;
  std::string_view _alpnProtocol;  // Selected ALPN protocol (if any) for this connection, empty if none/unsupported
  std::string_view _tlsCipher;     // Negotiated cipher suite (empty if not TLS)
  std::string_view _tlsVersion;    // Negotiated TLS protocol version (e.g. TLSv1.3) empty if not TLS

  std::chrono::steady_clock::time_point _reqStart;
  tracing::SpanPtr _traceSpan;
};

}  // namespace aeronet