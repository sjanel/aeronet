#pragma once

#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iterator>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

#include "aeronet/encoding.hpp"

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
#include <coroutine>
#include <functional>
#include <thread>
#endif

#include "aeronet/city-hash.hpp"
#include "aeronet/concatenated-headers.hpp"
#include "aeronet/headers-view-map.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/http-version.hpp"
#include "aeronet/path-param-capture.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/tracing/tracer.hpp"

namespace aeronet {

namespace internal {
class ConnectionStorage;
class HttpCodec;
struct ResponseCompressionState;
}  // namespace internal

struct ConnectionState;
struct HttpServerConfig;

namespace http2 {
class Http2ProtocolHandler;
}  // namespace http2

class HttpRequest {
 public:
  static constexpr std::size_t kDefaultReadBodyChunk = 4096;

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  class BodyChunkAwaitable {
   public:
    BodyChunkAwaitable(HttpRequest& request, std::size_t maxBytes) noexcept : _request(request), _maxBytes(maxBytes) {}

    [[nodiscard]] bool await_ready() const noexcept { return _request.isBodyReady(); }
    void await_suspend([[maybe_unused]] std::coroutine_handle<> coroutine) const noexcept {
      _request.markAwaitingBody();
    }

    [[nodiscard]] std::string_view await_resume() { return _request.readBody(_maxBytes); }

   private:
    HttpRequest& _request;
    std::size_t _maxBytes;
  };

  class BodyAggregateAwaitable {
   public:
    explicit BodyAggregateAwaitable(HttpRequest& request) noexcept : _request(request) {}

    [[nodiscard]] bool await_ready() const noexcept { return _request.isBodyReady(); }
    void await_suspend([[maybe_unused]] std::coroutine_handle<> coroutine) const noexcept {
      _request.markAwaitingBody();
    }

    [[nodiscard]] std::string_view await_resume() { return _request.body(); }

   private:
    HttpRequest& _request;
  };

  // DeferredWork: awaitable for running work on a background thread and resuming in the server's event loop.
  // This enables true async operations (database queries, API calls, file I/O) without blocking the event loop.
  //
  // Usage:
  //   auto result = co_await req.deferWork([&]() -> MyResult {
  //     // This lambda runs on a background thread
  //     return slowDatabaseQuery();  // blocking I/O is fine here
  //   });
  //
  // The coroutine suspends immediately, the work function executes on a new thread, and when complete,
  // the server's event loop is notified to resume the coroutine with the result.
  //
  // Exception handling: If the work function throws, the exception is captured and rethrown when
  // await_resume() is called, propagating it through the coroutine normally.
  template <typename Result>
  class DeferredWorkAwaitable {
   public:
    using WorkFn = std::function<Result()>;

    DeferredWorkAwaitable(HttpRequest& request, WorkFn work) noexcept : _request(request), _work(std::move(work)) {}

    [[nodiscard]] bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> handle) noexcept {
      _request.markAwaitingCallback();

      auto work = std::move(_work);
      Result* resultPtr = &_result;
      std::exception_ptr* exPtr = &_exception;

      std::thread([&req = _request, handle, work = std::move(work), resultPtr, exPtr]() mutable {
        try {
          *resultPtr = work();
        } catch (...) {
          *exPtr = std::current_exception();
        }
        req.postCallback(handle, nullptr);
      }).detach();
    }

    [[nodiscard]] Result await_resume() {
      if (_exception) {
        std::rethrow_exception(_exception);
      }
      return std::move(_result);
    }

   private:
    HttpRequest& _request;
    WorkFn _work;
    Result _result{};
    std::exception_ptr _exception;
  };
#endif

  // The method of the request (GET, PUT, ...)
  [[nodiscard]] http::Method method() const noexcept { return _method; }

  // The URL decoded path (the target without the query params string).
  // It cannot be empty.
  // Example:
  //  GET /path               -> '/path'
  //  GET /path?key=val       -> '/path'
  //  GET /path%2Caaa?key=val -> '/path,aaa'
  [[nodiscard]] std::string_view path() const noexcept { return {_pPath, _pathLength}; }

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
  [[nodiscard]] const HeadersViewMap& headers() const noexcept { return _headers; }

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
  [[nodiscard]] std::string_view headerValueOrEmpty(std::string_view headerKey) const noexcept {
    return headerValue(headerKey).value_or(std::string_view{});
  }

  // Like headerValueOrEmpty() but preserves the distinction between absence and an explicitly empty value.
  //   * std::nullopt  => header not present in the request.
  //   * engaged empty => header present with zero-length (after trimming) value.
  //   * engaged non-empty => possibly merged / override-normalized value (see duplicate handling above).
  // All trimming, merge, override, and lifetime notes from headerValueOrEmpty() apply here.
  // Use this when protocol logic must differentiate between omitted vs intentionally blank headers.
  [[nodiscard]] std::optional<std::string_view> headerValue(std::string_view headerKey) const noexcept {
    const auto it = _headers.find(headerKey);
    return it != _headers.end() ? std::optional<std::string_view>{it->second} : std::nullopt;
  }

  // Returns true if the given header is present (regardless of value).
  [[nodiscard]] bool hasHeader(std::string_view headerKey) const noexcept { return _headers.contains(headerKey); }

  // Returns a map-like view over the parsed & URL decoded query parameters.
  // - Duplicated keys are collapsed; only the last occurrence is retained.
  // - Key/value views point into the connection buffer; valid only during the handler call.
  // - The order of entries and duplicates is NOT preserved.
  // If you need to preserve order and manage duplicates, use queryParamsRange().
  [[nodiscard]] const auto& queryParams() const noexcept { return _queryParams; }

  // Returns true if the given query parameter key is present (regardless of value).
  [[nodiscard]] bool hasQueryParam(std::string_view key) const noexcept { return _queryParams.contains(key); }

  // Get the last value for the given query parameter key, or std::nullopt if not present.
  [[nodiscard]] std::optional<std::string_view> queryParamValue(std::string_view key) const noexcept {
    const auto it = _queryParams.find(key);
    return it != _queryParams.end() ? std::optional<std::string_view>{it->second} : std::nullopt;
  }

  // Convenient typed accessor for integer query parameters.
  // Returns std::nullopt if the key is not present or if the value cannot be parsed as an integer of the requested
  // type. Example:
  //   GET /path?count=42&invalid=abc
  //   auto count = req.queryParamInt("count");      // returns std::optional<int> with value 42
  //   auto invalid = req.queryParamInt("invalid");  //   // returns std::nullopt because "abc" is not a valid integer
  template <typename IntType = int>
  [[nodiscard]] std::optional<IntType> queryParamInt(std::string_view key) const noexcept {
    if (const auto valOpt = queryParamValue(key); valOpt) {
      IntType intVal;
      const auto [ptr, ec] = std::from_chars(valOpt->data(), valOpt->data() + valOpt->size(), intVal);
      if (ec == std::errc() && ptr == valOpt->data() + valOpt->size()) {
        return intVal;
      }
    }
    return std::nullopt;
  }

  // Like queryParamValue() but returns empty string_view if the key is not present.
  // To differentiate between absent and empty values, use queryParamValue().
  [[nodiscard]] std::string_view queryParamValueOrEmpty(std::string_view key) const noexcept {
    return queryParamValue(key).value_or(std::string_view{});
  }

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
      using difference_type = std::ptrdiff_t;
      using value_type = QueryParam;
      using reference = QueryParam;
      using iterator_category = std::forward_iterator_tag;

      iterator() noexcept : _begKey(nullptr), _endFullQuery(nullptr) {}

      iterator(const char* begKey, const char* endFullQuery) : _begKey(begKey), _endFullQuery(endFullQuery) {}

      QueryParam operator*() const;

      iterator& operator++() {
        advance();
        return *this;
      }

      iterator operator++(int) {
        auto ret = *this;
        advance();
        return ret;
      }

      bool operator==(iterator other) const noexcept { return _begKey == other._begKey; }

     private:
      void advance();

      const char* _begKey;
      const char* _endFullQuery;
    };

    [[nodiscard]] iterator begin() const noexcept { return {_first, _first + _length}; }
    [[nodiscard]] iterator end() const noexcept { return {_first + _length, _first + _length}; }

   private:
    friend class HttpRequest;

    QueryParamRange(const char* first, uint32_t length) noexcept : _first(first), _length(length) {}

    const char* _first;
    uint32_t _length;
  };

  // Get an iterable range on URL decoded query params.
  // The order of entries and duplicates are preserved.
  // This function is non-allocating.
  // Empty values are possible (missing '=' also results in empty value).
  // Example:
  //    GET /path?k=1&empty=&novalue&k=2
  //    for (const auto &[key, value] : httpRequest.queryParamsRange()) {
  //      // [0] key="k",       value="1"
  //      // [1] key="empty",   value=""
  //      // [2] key="novalue", value=""
  //      // [3] key="k",       value="2"
  //    }
  [[nodiscard]] QueryParamRange queryParamsRange() const noexcept {
    return {_pDecodedQueryParams, _decodedQueryParamsLength};
  }

  // Get the (already received) body of the request.
  // Throws if readBody() was previously called on this request.
  [[nodiscard]] std::string_view body() const;

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  // Awaitable helper returning the fully buffered body. Currently completes synchronously but exposes an
  // awaitable interface so coroutine-based handlers can share the same API surface as future streaming support.
  [[nodiscard]] BodyAggregateAwaitable bodyAwaitable() { return BodyAggregateAwaitable(*this); }

  // Defer work to a background thread and resume in the server's event loop when complete.
  // This is the idiomatic way to perform blocking operations (database queries, API calls, file I/O)
  // in async handlers without blocking the server's event loop.
  //
  // The work function executes on a detached thread. When it completes, the server's event loop
  // is notified and the coroutine resumes with the result.
  //
  // Usage:
  //   auto user = co_await req.deferWork([userId = std::string(userId)]() {
  //     return database.query("SELECT * FROM users WHERE id = ?", userId);
  //   });
  //
  // Thread safety: The work function runs on a background thread. Be careful with captured references.
  // Copy any data you need, or use thread-safe data structures.
  template <typename WorkFn>
  [[nodiscard]] auto deferWork(WorkFn&& work) {
    using Result = std::invoke_result_t<WorkFn>;
    return DeferredWorkAwaitable<Result>(*this, std::forward<WorkFn>(work));
  }

  // Awaitable helper for streaming body reads. Suspends cooperatively once real async body pipelines are wired; for
  // now it completes synchronously while providing a coroutine-friendly API surface.
  [[nodiscard]] BodyChunkAwaitable readBodyAsync(std::size_t maxBytes = kDefaultReadBodyChunk) {
    return {*this, maxBytes};
  }
#endif

  // Indicates whether additional body data remains to be read via readBody().
  [[nodiscard]] bool hasMoreBody() const;

  // Streaming accessor for the decoded request body. Returns a view that remains valid until the next readBody()
  // invocation or until the handler returns. Once an empty view is returned, the body (and any trailers) have been
  // fully consumed and subsequent calls will continue returning empty.
  // Preconditions:
  //   - hasMoreBody() must be true, otherwise behavior is undefined
  //   - body() must not have been called prior.
  [[nodiscard]] std::string_view readBody(std::size_t maxBytes = kDefaultReadBodyChunk);

  // Indicates whether the body is ready to be read (either fully buffered or streaming bridge established).
  [[nodiscard]] bool isBodyReady() const noexcept {
    return _bodyAccessBridge != nullptr || _bodyAccessMode == BodyAccessMode::Aggregated;
  }

  // Returns a map-like, case-insensitive view over trailer headers received after a chunked body (RFC 7230 §4.1.2).
  // Characteristics:
  //   * Only populated for chunked requests; empty for fixed Content-Length or bodyless requests.
  //   * Same duplicate-header merge policy as regular headers (comma-join, override, etc.).
  //   * Values are string_view slices into the connection buffer; valid only during the handler call.
  //   * Forbidden trailer fields (transfer-encoding, content-length, host, etc.) are rejected with 400.
  //   * Trailers count toward the maxHeadersBytes limit (combined with initial headers).
  [[nodiscard]] const HeadersViewMap& trailers() const noexcept { return _trailers; }

  // Like headerValueOrEmpty() but for trailers.
  [[nodiscard]] std::string_view trailerValueOrEmpty(std::string_view trailerKey) const noexcept {
    return trailerValue(trailerKey).value_or(std::string_view{});
  }

  // Like headerValue() but for trailers.
  [[nodiscard]] std::optional<std::string_view> trailerValue(std::string_view trailerKey) const noexcept {
    const auto it = _trailers.find(trailerKey);
    return it != _trailers.end() ? std::optional<std::string_view>{it->second} : std::nullopt;
  }

  // Returns true if the given trailer is present (regardless of value).
  [[nodiscard]] bool hasTrailer(std::string_view trailerKey) const noexcept { return _trailers.contains(trailerKey); }

  // Returns a map-like view over path parameters extracted during route matching.
  // Characteristics:
  //   * Key/value views point into the connection buffer; valid only during the handler call.
  //   * Values are already percent-decoded.
  //   * The order of entries is not specified.
  //   * If the patterns were unnamed, the keys are numeric strings representing the 0-based index of the match.
  [[nodiscard]] const auto& pathParams() const noexcept { return _pathParams; }

  // Returns true if the given path parameter key was captured.
  [[nodiscard]] bool hasPathParam(std::string_view key) const noexcept { return _pathParams.contains(key); }

  // Get the value for the given path parameter key, or std::nullopt if not present.
  // Captured path parameter values may be empty (zero-length). Both accessors below
  // are therefore useful: `pathParamValue()` preserves the distinction between
  // "absent" and "present-but-empty", while `pathParamValueOrEmpty()` conveniently
  // returns an empty `string_view` when the key is not present.
  [[nodiscard]] std::optional<std::string_view> pathParamValue(std::string_view key) const noexcept {
    const auto it = _pathParams.find(key);
    return it != _pathParams.end() ? std::optional<std::string_view>{it->second} : std::nullopt;
  }

  // Like `pathParamValue()` but returns an empty `string_view` if the key is not present.
  // Use this when a default empty view is preferred and you don't need to distinguish
  // between absent and explicitly empty captures.
  [[nodiscard]] std::string_view pathParamValueOrEmpty(std::string_view key) const noexcept {
    return pathParamValue(key).value_or(std::string_view{});
  }

  // Selected ALPN protocol (if negotiated); empty if none or not TLS.
  [[nodiscard]] std::string_view alpnProtocol() const noexcept;

  // Negotiated TLS cipher suite; empty if connection not using TLS.
  [[nodiscard]] std::string_view tlsCipher() const noexcept;

  // Negotiated TLS protocol version string (e.g. "TLSv1.3"); empty if not TLS.
  [[nodiscard]] std::string_view tlsVersion() const noexcept;

  // ============================
  // HTTP/2-specific accessors
  // ============================

  // Returns true if this request arrived over HTTP/2.
  [[nodiscard]] bool isHttp2() const noexcept { return _streamId != 0; }

  // HTTP/2 stream identifier (0 for HTTP/1.x requests).
  [[nodiscard]] uint32_t streamId() const noexcept { return _streamId; }

  // HTTP/2 :scheme pseudo-header ("https" or "http"); empty for HTTP/1.x.
  [[nodiscard]] std::string_view scheme() const noexcept { return {_pScheme, _schemeLength}; }

  // HTTP/2 :authority pseudo-header (equivalent to Host header); empty for HTTP/1.x.
  // For HTTP/1.x requests, use headerValueOrEmpty("Host") instead.
  [[nodiscard]] std::string_view authority() const noexcept { return {_pAuthority, _authorityLength}; }

  // Tells whether this request has a 'Expect: 100-continue' header.
  [[nodiscard]] bool hasExpectContinue() const noexcept;

  // Timestamp when request parsing began.
  [[nodiscard]] std::chrono::steady_clock::time_point reqStart() const noexcept { return _reqStart; }

  // Size of the request head span.
  // This is the sum of the lengths of the request line and all headers including CRLFs.
  [[nodiscard]] std::size_t headSpanSize() const noexcept { return _headSpanSize; }

  // ============================
  // Make Response helpers
  // ============================

  // Creates an HttpResponse with the given status code (200 by default).
  // Compared to the direct constructor, using this method may enable some optimizations,
  // as it prepares some work usually done at finalization step which avoids memory moves.
  // For instance, if you use global headers, the allocated memory will be correctly sized and
  // all HTTP response components correctly placed in the buffer from the start.
  // The returned HttpResponse can be further modified, but for best performance, avoid adding headers
  // after body like usual.
  [[nodiscard]] HttpResponse makeResponse(http::StatusCode statusCode = http::StatusCodeOK) const {
    return makeResponse(0UL, statusCode);
  }

  // Same as makeResponse(statusCode) but with additional capacity for the internal buffer.
  [[nodiscard]] HttpResponse makeResponse(std::size_t additionalCapacity, http::StatusCode statusCode) const;

  // Same as makeResponse(200) but also sets the body and content type.
  [[nodiscard]] HttpResponse makeResponse(std::string_view body,
                                          std::string_view contentType = http::ContentTypeTextPlain) const;

  // Same as makeResponse(statusCode) but also sets the body and content type.
  [[nodiscard]] HttpResponse makeResponse(http::StatusCode statusCode, std::string_view body,
                                          std::string_view contentType = http::ContentTypeTextPlain) const;

  // Same as makeResponse(200) but also sets the body from given bytes span and content type.
  [[nodiscard]] HttpResponse makeResponse(std::span<const std::byte> body,
                                          std::string_view contentType = http::ContentTypeApplicationOctetStream) const;

  // Same as makeResponse(statusCode) but also sets the body from given bytes span and content type.
  [[nodiscard]] HttpResponse makeResponse(http::StatusCode statusCode, std::span<const std::byte> body,
                                          std::string_view contentType = http::ContentTypeApplicationOctetStream) const;

  // Returns the best encoding that can be used for the response based on the
  // Accept-Encoding header of the request and the server compression configuration.
  [[nodiscard]] Encoding responsePossibleEncoding() const noexcept { return _responsePossibleEncoding; }

 private:
  friend class SingleHttpServer;
  friend class HttpRequestTest;
  friend class StaticFileHandlerTest;
  friend class CorsPolicyTest;
  friend class UpgradeHandlerHarness;
  friend struct ConnectionState;
  friend class internal::ConnectionStorage;
#ifdef AERONET_ENABLE_HTTP2
  friend class http2::Http2ProtocolHandler;
#endif
  friend class internal::HttpCodec;

  static constexpr http::StatusCode kStatusNeedMoreData = static_cast<http::StatusCode>(0);

  HttpRequest() noexcept = default;

  [[nodiscard]] bool wantClose() const;

  [[nodiscard]] bool isKeepAliveForHttp1(bool enableKeepAlive, uint32_t maxRequestsPerConnection,
                                         bool isServerRunning) const;

  void init(const HttpServerConfig& config, internal::ResponseCompressionState& compressionState);

  // Attempts to set this HttpRequest (except body) from given ConnectionState.
  // Returns StatusCode OK if the request is good (it will be fully set) or an HTTP error status to forward.
  // If 0 is returned, it means the connection state buffer is not filled up to the first newline.
  http::StatusCode initTrySetHead(std::span<char> inBuffer, RawChars& tmpBuffer, std::size_t maxHeadersBytes,
                                  bool mergeAllowedForUnknownRequestHeaders, tracing::SpanPtr traceSpan);

  void finalizeBeforeHandlerCall(std::span<const PathParamCapture> pathParams);

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  void pinHeadStorage(ConnectionState& state);
#endif

  void shrinkAndMaybeClear();

  void end(http::StatusCode respStatusCode);

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  void markAwaitingBody() const noexcept;
  void markAwaitingCallback() const noexcept;

  // Post a callback to be executed in the server's event loop, then resume the coroutine.
  void postCallback(std::coroutine_handle<> handle, std::function<void()> work) const;

  // HTTP/2 async handler support: alternative callback mechanism for per-stream async tasks.
  // When set, markAwaitingCallback() / postCallback() use these instead of _ownerState->asyncState.
  using H2PostCallbackFn = std::function<void(std::coroutine_handle<>, std::function<void()>)>;
  H2PostCallbackFn _h2PostCallback;
  bool* _h2SuspendedFlag{nullptr};
#endif

  [[nodiscard]] HttpResponse::Options makeResponseOptions() const noexcept;

  // Decodes the path in-place and sets _pPath and _pathLength. Returns false if the path is malformed (e.g. invalid
  // percent-encoding).
  bool decodePath(char* pathStart, char* pathEnd);

  enum class BodyAccessMode : uint8_t { Undecided, Streaming, Aggregated };
  struct BodyAccessBridge {
    using AggregateFn = std::string_view (*)(HttpRequest&, void* context);
    using ReadChunkFn = std::string_view (*)(HttpRequest&, void* context, std::size_t maxBytes);
    using HasMoreFn = bool (*)(const HttpRequest&, void* context);

    AggregateFn aggregate{nullptr};
    ReadChunkFn readChunk{nullptr};
    HasMoreFn hasMore{nullptr};
  };

  const char* _pPath{nullptr};
  const char* _pScheme{nullptr};     // :scheme pseudo-header ("http" or "https")
  const char* _pAuthority{nullptr};  // :authority pseudo-header (equivalent to Host)
  // Raw query component (excluding '?') retained as-is; per-key/value decoding happens on iteration.
  const char* _pDecodedQueryParams{nullptr};
  const ConcatenatedHeaders* _pGlobalHeaders{nullptr};

  HeadersViewMap _headers;
  HeadersViewMap _trailers;  // Trailer headers (RFC 7230 §4.1.2) from chunked requests
  flat_hash_map<std::string_view, std::string_view, CityHash> _pathParams;
  flat_hash_map<std::string_view, std::string_view, CityHash> _queryParams;

  std::string_view _body;
  std::string_view _activeStreamingChunk;
  const BodyAccessBridge* _bodyAccessBridge{nullptr};
  void* _bodyAccessContext{nullptr};
  ConnectionState* _ownerState{nullptr};
  internal::ResponseCompressionState* _pCompressionState{nullptr};

  std::chrono::steady_clock::time_point _reqStart;
  std::size_t _headSpanSize{0};
  tracing::SpanPtr _traceSpan;
  uint32_t _streamId{0};  // HTTP/2 stream ID (0 indicates HTTP/1.x)
  uint32_t _pathLength{0};
  uint32_t _schemeLength{0};
  uint32_t _authorityLength{0};
  uint32_t _decodedQueryParamsLength{0};
  http::Version _version;
  http::Method _method;
  BodyAccessMode _bodyAccessMode{BodyAccessMode::Undecided};
  Encoding _responsePossibleEncoding{Encoding::none};
  bool _headPinned{false};
  bool _addTrailerHeader{false};
  bool _addVaryAcceptEncoding{false};
};

}  // namespace aeronet