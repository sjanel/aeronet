#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aeronet/http-body.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-header.hpp"
#include "aeronet/http-response-data.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/http-version.hpp"
#include "raw-chars.hpp"
#include "simple-charconv.hpp"
#include "timedef.hpp"

namespace aeronet {

// -----------------------------------------------------------------------------
// HttpResponse
// -----------------------------------------------------------------------------
// A contiguous single-buffer HTTP/1.x response builder focused on minimal
// allocations and cache-friendly writes, optionally supporting large bodies captured
// in the response.
//
// Memory layout (before finalize):
//   [HTTP/1.x SP status-code [SP reason] CRLF][CRLF][CRLF]  (DoubleCRLF sentinel)
//   ^             ^             ^           ^   ^
//   |             |             |           |   +-- part of DoubleCRLF
//   |             |             |           +------ end of status/optional reason line
//   |             |             +-- beginning of reason
//   |             +-- beginning of status code
//   +-- start
//
// After headers are appended:
//   Status/Reason CRLF (CRLF HeaderName ": " Value)* CRLF CRLF [Body]
//   (optional) Trailer lines may follow the body when present:
//     CRLF Trailer-Name ": " Value CRLF ... CRLF  (trailers appear after the body)
//
// Header Insertion Strategy:
//   Each user header is inserted as: CRLF + name + ": " + value (no trailing CRLF).
//   The leading CRLF acts as the line terminator for either the status line (first
//   header inserted) or the previous header. The final DoubleCRLF already present
//   at construction terminates the header block. This lets us append headers by
//   shifting only the tail (DoubleCRLF + body) once per insertion.
//
//   addCustomHeader():
//     - O(T) memmove of tail where T = size(DoubleCRLF + current body), no scan of
//       existing headers (fast path). Allows duplicates intentionally.
//
//   customHeader():
//     - Linear scan of current header region to find existing key at line starts
//       (recognised by preceding CRLF). If found, value replaced in-place adjusting
//       buffer via single memmove for size delta. If not found, falls back to append.
//     - Because of the scan it is less efficient than addCustomHeader(). Prefer
//       addCustomHeader() when duplicates are acceptable or order-only semantics matter.
//
// Mutators & Finalization:
//   statusCode(), reason(), body(), addCustomHeader(), customHeader() may be called in any
//   order prior to finalizeAndGetFullTextResponse(). finalize* injects reserved
//   headers (Content-Length if body non-empty, Date, Connection) every time it is
//   called; therefore call it exactly once. Post-finalization mutation is NOT
//   supported and will produce duplicated reserved headers.
//
// Reserved Headers (user cannot set): Date, Connection, Content-Length,
//   Transfer-Encoding, Trailer, Upgrade, TE.
//
// Complexity Summary:
//   - statusCode(): O(1)
//   - reason(): O(size of tail - adjusts headers/body offsets)
//   - body(): O(delta) for copy; may reallocate
//   - addCustomHeader(): O(bodyLen) for memmove of tail
//   - customHeader(): O(totalHeaderBytes) scan + O(bodyLen) memmove if size delta
//
// Safety & Assumptions:
//   - Not thread-safe.
//   - Throws std::bad_alloc on growth failure.
//   - Assumes ASCII header names; no validation performed.
//   - Trailers can only be added after body final set (no more body modification can happen once a trailer has been
//   added)
//
// Performance hints:
//   - Appends HttpResponse data in order of the HTTP layout (reason, headers, body) to minimize data movement.
//   - Prefer addCustomHeader() when duplicates are acceptable or order-only semantics matter.
//   - Minimize header mutations after body() to reduce data movement.
//
// Trailers (outbound / response-side):
//   - HttpResponse supports adding trailer headers that will be transmitted after the
//     response body when the response is serialized. Trailers are intended for metadata
//     computed after body generation (checksums, signatures, processing totals, etc.).
//   - Ordering constraint: trailers MUST be added after the body has been set (via
//     any `body()` overload). This requirement enables a zero-allocation implementation
//     where trailer text is appended directly to the existing body buffer.
//   - Zero-allocation design: when `addTrailer()` is called the implementation appends
//     `CRLF + name + ": " + value` lines directly to the tail buffer that holds the
//     body. This avoids an extra heap allocation for trailer storage. For large bodies
//     that were captured into `_capturedBody`, trailers are appended into the captured
//     body storage prior to finalize/steal. For inline bodies they are appended into
//     `_data` after the existing body bytes.
//   - Finalization: `finalizeAndStealData()` is responsible for injecting any required
//     reserved headers (Date, Connection, Content-Length when appropriate) and for
//     returning the body buffer which already contains any appended trailer text. After
//     finalization the HttpResponse instance must not be reused.
//   - Complexity: appending a trailer is O(bodyLen) in the worst case due to the
//     potential memmove of the tail (same complexity as adding a header after the body).
//   - Streaming responses: `HttpResponseWriter` implements a separate streaming-safe
//     `addTrailer()` API which buffers trailer lines during streaming and emits them
//     after the final zero-length chunk (see `HttpResponseWriter` docs).
// -----------------------------------------------------------------------------
class HttpResponse {
 private:
  // "HTTP/x.y". Should be changed if version major / minor exceed 1 digit
  static constexpr std::size_t kHttp1VersionLen = http::HTTP10Sv.size();
  static constexpr std::size_t kStatusCodeBeg = kHttp1VersionLen + 1;  // index of first status code digit
  static constexpr std::size_t kReasonBeg = kStatusCodeBeg + 3 + 1;    // index of first reason phrase character

 public:
  explicit HttpResponse(http::StatusCode code = 200, std::string_view reason = {});

  // Replaces the status code. Must be a 3 digits integer.
  HttpResponse& statusCode(http::StatusCode statusCode) & noexcept {
    assert(statusCode >= 100 && statusCode < 1000);
    write3(_data.data() + kStatusCodeBeg, statusCode);
    return *this;
  }

  // Replaces the status code. Must be a 3 digits integer.
  HttpResponse&& statusCode(http::StatusCode statusCode) && noexcept {
    assert(statusCode >= 100 && statusCode < 1000);
    write3(_data.data() + kStatusCodeBeg, statusCode);
    return std::move(*this);
  }

  // Get the current status code stored in this HttpResponse.
  [[nodiscard]] http::StatusCode statusCode() const noexcept {
    return static_cast<http::StatusCode>(read3(_data.data() + kStatusCodeBeg));
  }

  // Sets or replace the reason phrase for this instance.
  // Inserting empty reason is allowed.
  // If the data to be inserted references internal instance memory, the behavior is undefined.
  HttpResponse& reason(std::string_view reason) & {
    setReason(reason);
    return *this;
  }

  // Sets or replace the reason phrase for this instance.
  // Inserting empty reason is allowed.
  // If the data to be inserted references internal instance memory, the behavior is undefined.
  HttpResponse&& reason(std::string_view reason) && {
    setReason(reason);
    return std::move(*this);
  }

  // Get the current reason stored in this HttpResponse.
  [[nodiscard]] std::string_view reason() const noexcept { return {_data.data() + kReasonBeg, reasonLen()}; }

  // Assigns the given body to this HttpResponse.
  // Empty body is allowed.
  // The whole buffer is copied internally in the HttpResponse. If the body is large, prefer the capture by value of
  // body() overloads to avoid a copy (and possibly an allocation).
  // Body referencing internal memory of this HttpResponse is allowed as well.
  // Example:
  //   HttpResponse resp(404, "Not Found");
  //   resp.body(resp.reason()); // OK
  HttpResponse& body(std::string_view body) & {
    setBodyInternal(body);
    _capturedBody = {};
    return *this;
  }

  // Assigns the given body to this HttpResponse.
  // Empty body is allowed.
  // The whole buffer is copied internally in the HttpResponse. If the body is large, prefer the capture by value of
  // body() overloads to avoid a copy (and possibly an allocation).
  // Body referencing internal memory of this HttpResponse is allowed as well.
  // Example:
  //   HttpResponse resp(404, "Not Found");
  //   resp.body(resp.reason()); // OK
  HttpResponse&& body(std::string_view body) && {
    setBodyInternal(body);
    _capturedBody = {};
    return std::move(*this);
  }

  // Assigns the given body to this HttpResponse.
  // Empty body is allowed.
  // The whole buffer is copied internally in the HttpResponse. If the body is large, prefer the capture by value of
  // body() overloads to avoid a copy (and possibly an allocation).
  // Body referencing internal memory of this HttpResponse is allowed as well.
  // Example:
  //   HttpResponse resp(404, "Not Found");
  //   resp.body(resp.reason()); // OK
  HttpResponse& body(const char* body) & {
    setBodyInternal(body == nullptr ? std::string_view() : std::string_view(body));
    _capturedBody = {};
    return *this;
  }

  // Assigns the given body to this HttpResponse.
  // Empty body is allowed.
  // The whole buffer is copied internally in the HttpResponse. If the body is large, prefer the capture by value of
  // body() overloads to avoid a copy (and possibly an allocation).
  // Body referencing internal memory of this HttpResponse is allowed as well.
  // Example:
  //   HttpResponse resp(404, "Not Found");
  //   resp.body(resp.reason()); // OK
  HttpResponse&& body(const char* body) && {
    setBodyInternal(body == nullptr ? std::string_view() : std::string_view(body));
    _capturedBody = {};
    return std::move(*this);
  }

  // Capture the body by value to avoid a copy (and possibly an allocation).
  // Empty body is allowed.
  // The body instance is moved into this HttpResponse.
  HttpResponse& body(std::string body) & noexcept {
    setBodyInternal({});
    _capturedBody = HttpBody(std::move(body));
    return *this;
  }

  // Capture the body by value to avoid a copy (and possibly an allocation).
  // Empty body is allowed.
  // The body instance is moved into this HttpResponse.
  HttpResponse&& body(std::string body) && noexcept {
    setBodyInternal({});
    _capturedBody = HttpBody(std::move(body));
    return std::move(*this);
  }

  // Capture the body by value to avoid a copy (and possibly an allocation).
  // Empty body is allowed.
  // The body instance is moved into this HttpResponse.
  HttpResponse& body(std::vector<char> body) & noexcept {
    setBodyInternal({});
    _capturedBody = HttpBody(std::move(body));
    return *this;
  }

  // Capture the body by value to avoid a copy (and possibly an allocation).
  // Empty body is allowed.
  // The body instance is moved into this HttpResponse.
  HttpResponse&& body(std::vector<char> body) && noexcept {
    setBodyInternal({});
    _capturedBody = HttpBody(std::move(body));
    return std::move(*this);
  }

  // Capture the body by value to avoid a copy (and possibly an allocation).
  // Empty body is allowed.
  // The body instance is moved into this HttpResponse.
  HttpResponse& body(std::unique_ptr<char[]> body, std::size_t size) & noexcept {
    setBodyInternal({});
    _capturedBody = HttpBody(std::move(body), size);
    return *this;
  }

  // Capture the body by value to avoid a copy (and possibly an allocation).
  // Empty body is allowed.
  // The body instance is moved into this HttpResponse.
  HttpResponse&& body(std::unique_ptr<char[]> body, std::size_t size) && noexcept {
    setBodyInternal({});
    _capturedBody = HttpBody(std::move(body), size);
    return std::move(*this);
  }

  // Get a view of the current body stored in this HttpResponse.
  // If the body is not present, it returns an empty view.
  [[nodiscard]] std::string_view body() const noexcept {
    auto ret =
        _capturedBody.set() ? _capturedBody.view() : std::string_view{_data.begin() + _bodyStartPos, _data.end()};
    if (_trailerPos != 0) {
      ret.remove_suffix(ret.size() - _trailerPos);
    }
    return ret;
  }

  // Inserts or replaces the Content-Type header.
  // If the data to be inserted references internal instance memory, the behavior is undefined.
  HttpResponse& contentType(std::string_view src) & { return customHeader(http::ContentType, src); }

  // Inserts or replaces the Content-Type header.
  // If the data to be inserted references internal instance memory, the behavior is undefined.
  HttpResponse&& contentType(std::string_view src) && {
    customHeader(http::ContentType, src);
    return std::move(*this);
  }

  // Inserts or replaces the Location header.
  // If the data to be inserted references internal instance memory, the behavior is undefined.
  HttpResponse& location(std::string_view src) & { return customHeader(http::Location, src); }

  // Inserts or replaces the Location header.
  // If the data to be inserted references internal instance memory, the behavior is undefined.
  HttpResponse&& location(std::string_view src) && {
    customHeader(http::Location, src);
    return std::move(*this);
  }

  // Inserts or replaces the Content-Encoding header.
  // If the data to be inserted references internal instance memory, the behavior is undefined.
  HttpResponse& contentEncoding(std::string_view src) & { return customHeader(http::ContentEncoding, src); }

  // Inserts or replaces the Content-Encoding header.
  // If the data to be inserted references internal instance memory, the behavior is undefined.
  HttpResponse&& contentEncoding(std::string_view src) && {
    customHeader(http::ContentEncoding, src);
    return std::move(*this);
  }

  // Append a header line (duplicates allowed, fastest path).
  // No scan over existing headers. Prefer this when duplicates are OK or
  // when constructing headers once.
  // Do not insert any reserved header (for which IsReservedResponseHeader is true), doing so is undefined behavior.
  // If the data to be inserted references internal instance memory, the behavior is undefined.
  HttpResponse& addCustomHeader(std::string_view key, std::string_view value) & {
    assert(!http::IsReservedResponseHeader(key));
    appendHeaderUnchecked(key, value);
    return *this;
  }

  // Append a header line (duplicates allowed, fastest path).
  // No scan over existing headers. Prefer this when duplicates are OK or
  // when constructing headers once.
  // Do not insert any reserved header (for which IsReservedResponseHeader is true), doing so is undefined behavior.
  // If the data to be inserted references internal instance memory, the behavior is undefined.
  HttpResponse&& addCustomHeader(std::string_view key, std::string_view value) && {
    assert(!http::IsReservedResponseHeader(key));
    appendHeaderUnchecked(key, value);
    return std::move(*this);
  }

  // Set or replace a header value ensuring at most one instance.
  // Performs a linear scan (slower than addCustomHeader()) using case-insensitive comparison of header names per
  // RFC 7230 (HTTP field names are case-insensitive). The original casing of the first occurrence is preserved.
  // If not found, falls back to addCustomHeader(). Use only when you must guarantee uniqueness; otherwise prefer
  // addCustomHeader().
  // Do not insert any reserved header (for which IsReservedResponseHeader is true), doing so is undefined behavior.
  // If the data to be inserted references internal instance memory, the behavior is undefined.
  HttpResponse& customHeader(std::string_view key, std::string_view value) & {
    assert(!http::IsReservedResponseHeader(key));
    setHeader(key, value, false);
    return *this;
  }

  // Set or replace a header value ensuring at most one instance.
  // Performs a linear scan (slower than addCustomHeader()) using case-insensitive comparison of header names per
  // RFC 7230 (HTTP field names are case-insensitive). The original casing of the first occurrence is preserved.
  // If not found, falls back to addCustomHeader(). Use only when you must guarantee uniqueness; otherwise prefer
  // addCustomHeader().
  // Do not insert any reserved header (for which IsReservedResponseHeader is true), doing so is undefined behavior.
  // If the data to be inserted references internal instance memory, the behavior is undefined.
  HttpResponse&& customHeader(std::string_view key, std::string_view value) && {
    assert(!http::IsReservedResponseHeader(key));
    setHeader(key, value, false);
    return std::move(*this);
  }

  // Whether user explicitly provided a Content-Encoding header (any value). When present
  // aeronet will NOT perform automatic compression for this response (the user fully
  // controls encoding and must ensure body matches the declared encoding). Users can
  // force identity / disable compression by setting either:
  //   Content-Encoding: identity
  // or an empty value ("\r\nContent-Encoding: \r\n") though the former is preferred.
  [[nodiscard]] bool userProvidedContentEncoding() const noexcept { return _userProvidedContentEncoding; }

  // Adds a trailer header to be sent after the response body (RFC 7230 §4.1.2).
  //
  // IMPORTANT ORDERING CONSTRAINT:
  //   Trailers MUST be added AFTER the body has been set (via body() or its overloads).
  //   If called before body is set, throws std::logic_error.
  //
  // Rationale:
  //   To avoid additional allocations, trailers are appended directly to the body buffer:
  //   This zero-allocation design requires the body to be finalized first.
  //
  // Trailer semantics (per RFC 7230 §4.1.2):
  //   - Trailers are sent after the message body in chunked transfer encoding.
  //   - Certain headers MUST NOT appear as trailers (e.g., Transfer-Encoding, Content-Length,
  //     Host, Cache-Control, Authorization, Cookie, Set-Cookie). Use of forbidden trailer
  //     headers is undefined behavior (no validation is performed here for performance;
  //     validation may be added in debug builds).
  //   - Typical use: computed metadata available only after body generation (checksums,
  //     signatures, etc.).
  //
  // Usage example:
  //   HttpResponse resp(200);
  //   resp.body("Wikipedia in\r\n\r\nchunks");
  //   resp.addTrailer("X-Checksum", "abc123");           // OK: body set first
  //   resp.addTrailer("X-Signature", "sha256:...");      // OK: multiple trailers allowed
  //   // resp.addTrailer("Host", "example.com");         // UNDEFINED: forbidden trailer
  HttpResponse& addTrailer(std::string_view name, std::string_view value) &;
  HttpResponse&& addTrailer(std::string_view name, std::string_view value) &&;

 private:
  friend class HttpServer;
  friend class HttpResponseTest;
  friend class HttpResponseWriter;  // streaming writer needs access to finalize

  [[nodiscard]] std::size_t reasonLen() const noexcept {
    if (_data[kReasonBeg] == '\n') {
      return 0UL;
    }
    if (_headersStartPos != 0) {
      return _headersStartPos - kReasonBeg;
    }
    return _bodyStartPos - kReasonBeg - http::DoubleCRLF.size();
  }

  [[nodiscard]] std::size_t bodyLen() const noexcept {
    if (_trailerPos != 0) {
      return _trailerPos;
    }
    return _capturedBody.set() ? _capturedBody.size() : internalBodyAndTrailersLen();
  }

  [[nodiscard]] std::size_t internalBodyAndTrailersLen() const noexcept { return _data.size() - _bodyStartPos; }

  void setReason(std::string_view newReason);

  void setBodyInternal(std::string_view newBody);

  void setHeader(std::string_view key, std::string_view value, bool onlyIfNew = false);

  void appendHeaderUnchecked(std::string_view key, std::string_view value);

  void appendDateUnchecked(SysTimePoint tp);

  void appendTrailer(std::string_view name, std::string_view value);

  // IMPORTANT: This method finalizes the response by appending reserved headers,
  // and returns the internal buffers stolen from this HttpResponse instance.
  // So this instance must not be used anymore after this call.
  HttpResponseData finalizeAndStealData(http::Version version, SysTimePoint tp, bool keepAlive,
                                        std::span<const http::Header> globalHeaders, bool isHeadMethod,
                                        std::size_t minCapturedBodySize);

  RawChars _data;
  uint16_t _headersStartPos{};  // position just at the CRLF that starts the first header line
  bool _userProvidedContentEncoding{false};
  uint32_t _bodyStartPos{};  // position of first body byte (after CRLF CRLF)
  HttpBody _capturedBody;
  std::size_t _trailerPos{};  // trailer pos in relative to body start
};

}  // namespace aeronet