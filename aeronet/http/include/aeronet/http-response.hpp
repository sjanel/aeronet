#pragma once

#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "aeronet/http-constants.hpp"
#include "aeronet/http-header.hpp"
#include "aeronet/http-payload.hpp"
#include "aeronet/http-response-data.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/http-version.hpp"
#include "file.hpp"
#include "raw-chars.hpp"
#include "simple-charconv.hpp"
#include "stringconv.hpp"
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
//   addHeader():
//     - O(T) memmove of tail where T = size(DoubleCRLF + current body), no scan of
//       existing headers (fast path). Allows duplicates intentionally.
//
//   header():
//     - Linear scan of current header region to find existing key at line starts
//       (recognised by preceding CRLF). If found, value replaced in-place adjusting
//       buffer via single memmove for size delta. If not found, falls back to append.
//     - Because of the scan it is less efficient than addHeader(). Prefer
//       addHeader() when duplicates are acceptable or order-only semantics matter.
//
// Mutators & Finalization:
//   status(), reason(), body(), addHeader(), header() may be called in any
//   order prior to finalizeAndGetFullTextResponse(). finalize* injects reserved
//   headers (Content-Length if body non-empty, Date, Connection) every time it is
//   called; therefore call it exactly once. Post-finalization mutation is NOT
//   supported and will produce duplicated reserved headers.
//
// Reserved Headers (user cannot set): Date, Connection, Content-Length,
//   Transfer-Encoding, Trailer, Upgrade, TE.
//
// Complexity Summary:
//   - status(): O(1)
//   - reason(): O(size of tail - adjusts headers/body offsets)
//   - body(): O(delta) for copy; may reallocate
//   - addHeader(): O(bodyLen) for memmove of tail
//   - header(): O(totalHeaderBytes) scan + O(bodyLen) memmove if size delta
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
//   - Prefer addHeader() when duplicates are acceptable or order-only semantics matter.
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
  enum class PayloadKind : uint8_t { Inline, Captured, File };

  // "HTTP/x.y". Should be changed if version major / minor exceed 1 digit
  static constexpr std::size_t kHttp1VersionLen = http::HTTP10Sv.size();
  static constexpr std::size_t kStatusCodeBeg = kHttp1VersionLen + 1;  // index of first status code digit
  static constexpr std::size_t kReasonBeg = kStatusCodeBeg + 3 + 1;    // index of first reason phrase character

 public:
  static constexpr std::size_t kHttpResponseMinInitialCapacity = 128UL;

  // Constructs an HttpResponse with the given status code and optional reason phrase, and a default initial capacity.
  explicit HttpResponse(http::StatusCode code = http::StatusCodeOK, std::string_view reason = {});

  // Constructs an HttpResponse with an initial capacity for the internal buffer, a status code and an optional reason
  // phrase. The initial capacity is rounded up to at least kHttpResponseMinInitialCapacity.
  // The capacity will hold at least the status line and the headers, and possibly the inlined body.
  HttpResponse(std::size_t initialCapacity, http::StatusCode code, std::string_view reason = {});

  // Replaces the status code. Must be a 3 digits integer (undefined behavior otherwise).
  HttpResponse& status(http::StatusCode statusCode) & noexcept {
    setStatusCode(statusCode);
    return *this;
  }

  // Replaces the status code. Must be a 3 digits integer (undefined behavior otherwise).
  HttpResponse&& status(http::StatusCode statusCode) && noexcept {
    setStatusCode(statusCode);
    return std::move(*this);
  }

  // Replaces the status code and the reason phrase. Must be a 3 digits integer (undefined behavior otherwise).
  HttpResponse& status(http::StatusCode statusCode, std::string_view reason) & noexcept {
    setStatusCode(statusCode);
    setReason(reason);
    return *this;
  }

  // Replaces the status code and the reason phrase. Must be a 3 digits integer (undefined behavior otherwise).
  HttpResponse&& status(http::StatusCode statusCode, std::string_view reason) && noexcept {
    setStatusCode(statusCode);
    setReason(reason);
    return std::move(*this);
  }

  // Get the current status code stored in this HttpResponse.
  [[nodiscard]] http::StatusCode status() const noexcept {
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
  HttpResponse& body(std::string_view body, std::string_view contentType = http::ContentTypeTextPlain) & {
    setBodyInternal(body);
    setContentTypeHeader(contentType, body.empty());
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
  HttpResponse&& body(std::string_view body, std::string_view contentType = http::ContentTypeTextPlain) && {
    setBodyInternal(body);
    setContentTypeHeader(contentType, body.empty());
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
  HttpResponse& body(std::span<const std::byte> body,
                     std::string_view contentType = http::ContentTypeApplicationOctetStream) & {
    return this->body(std::string_view{reinterpret_cast<const char*>(body.data()), body.size()}, contentType);
  }

  // Assigns the given body to this HttpResponse.
  // Empty body is allowed.
  // The whole buffer is copied internally in the HttpResponse. If the body is large, prefer the capture by value of
  // body() overloads to avoid a copy (and possibly an allocation).
  // Body referencing internal memory of this HttpResponse is allowed as well.
  // Example:
  //   HttpResponse resp(404, "Not Found");
  //   resp.body(resp.reason()); // OK
  HttpResponse&& body(std::span<const std::byte> body,
                      std::string_view contentType = http::ContentTypeApplicationOctetStream) && {
    return std::move(
        this->body(std::string_view{reinterpret_cast<const char*>(body.data()), body.size()}, contentType));
  }

  // Assigns the given body to this HttpResponse. The pointer MUST be null-terminated (or nullptr to empty body).
  // Empty body is allowed (Both "" and nullptr will be considered as empty body).
  // The whole buffer is copied internally in the HttpResponse. If the body is large, prefer the capture by value of
  // body() overloads to avoid a copy (and possibly an allocation).
  // Body referencing internal memory of this HttpResponse is allowed as well.
  // Example:
  //   HttpResponse resp(404, "Not Found");
  //   resp.body(resp.reason()); // OK
  HttpResponse& body(const char* body, std::string_view contentType = http::ContentTypeTextPlain) & {
    auto sv = body == nullptr ? std::string_view() : std::string_view(body);
    setBodyInternal(sv);
    setContentTypeHeader(contentType, sv.empty());
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
  HttpResponse&& body(const char* body, std::string_view contentType = http::ContentTypeTextPlain) && {
    auto sv = body == nullptr ? std::string_view() : std::string_view(body);
    setBodyInternal(sv);
    setContentTypeHeader(contentType, sv.empty());
    return std::move(*this);
  }

  // Capture the body by value to avoid a copy (and possibly an allocation).
  // Empty body is allowed.
  // The body instance is moved into this HttpResponse.
  HttpResponse& body(std::string body, std::string_view contentType = http::ContentTypeTextPlain) & {
    setBodyInternal(std::string_view{});
    setContentTypeHeader(contentType, body.empty());
    _payloadVariant = HttpPayload(std::move(body));
    _payloadKind = PayloadKind::Captured;
    return *this;
  }

  // Capture the body by value to avoid a copy (and possibly an allocation).
  // Empty body is allowed.
  // The body instance is moved into this HttpResponse.
  HttpResponse&& body(std::string body, std::string_view contentType = http::ContentTypeTextPlain) && {
    setBodyInternal(std::string_view{});
    setContentTypeHeader(contentType, body.empty());
    _payloadVariant = HttpPayload(std::move(body));
    _payloadKind = PayloadKind::Captured;
    return std::move(*this);
  }

  // Capture the body by value to avoid a copy (and possibly an allocation).
  // Empty body is allowed.
  // The body instance is moved into this HttpResponse.
  HttpResponse& body(std::vector<char> body, std::string_view contentType = http::ContentTypeApplicationOctetStream) & {
    setBodyInternal(std::string_view{});
    setContentTypeHeader(contentType, body.empty());
    _payloadVariant = HttpPayload(std::move(body));
    _payloadKind = PayloadKind::Captured;
    return *this;
  }

  // Capture the body by value to avoid a copy (and possibly an allocation).
  // Empty body is allowed.
  // The body instance is moved into this HttpResponse.
  HttpResponse&& body(std::vector<std::byte> body,
                      std::string_view contentType = http::ContentTypeApplicationOctetStream) && {
    setBodyInternal(std::string_view{});
    setContentTypeHeader(contentType, body.empty());
    _payloadVariant = HttpPayload(std::move(body));
    _payloadKind = PayloadKind::Captured;
    return std::move(*this);
  }

  // Capture the body by value to avoid a copy (and possibly an allocation).
  // Empty body is allowed.
  // The body instance is moved into this HttpResponse.
  HttpResponse& body(std::vector<std::byte> body,
                     std::string_view contentType = http::ContentTypeApplicationOctetStream) & {
    setBodyInternal(std::string_view{});
    setContentTypeHeader(contentType, body.empty());
    _payloadVariant = HttpPayload(std::move(body));
    _payloadKind = PayloadKind::Captured;
    return *this;
  }

  // Capture the body by value to avoid a copy (and possibly an allocation).
  // Empty body is allowed.
  // The body instance is moved into this HttpResponse.
  HttpResponse&& body(std::vector<char> body,
                      std::string_view contentType = http::ContentTypeApplicationOctetStream) && {
    setBodyInternal(std::string_view{});
    setContentTypeHeader(contentType, body.empty());
    _payloadVariant = HttpPayload(std::move(body));
    _payloadKind = PayloadKind::Captured;
    return std::move(*this);
  }

  // Capture the body by value to avoid a copy (and possibly an allocation).
  // Empty body is allowed.
  // The body instance is moved into this HttpResponse.
  HttpResponse& body(std::unique_ptr<char[]> body, std::size_t size,
                     std::string_view contentType = http::ContentTypeApplicationOctetStream) & {
    setBodyInternal(std::string_view{});
    setContentTypeHeader(contentType, size == 0);
    _payloadVariant = HttpPayload(std::move(body), size);
    _payloadKind = PayloadKind::Captured;
    return *this;
  }

  // Capture the body by value to avoid a copy (and possibly an allocation).
  // Empty body is allowed.
  // The body instance is moved into this HttpResponse.
  HttpResponse&& body(std::unique_ptr<char[]> body, std::size_t size,
                      std::string_view contentType = http::ContentTypeApplicationOctetStream) && {
    setBodyInternal(std::string_view{});
    setContentTypeHeader(contentType, size == 0);
    _payloadVariant = HttpPayload(std::move(body), size);
    _payloadKind = PayloadKind::Captured;
    return std::move(*this);
  }

  // Capture the body by value to avoid a copy (and possibly an allocation).
  // Empty body is allowed.
  // The body instance is moved into this HttpResponse.
  HttpResponse& body(std::unique_ptr<std::byte[]> body, std::size_t size,
                     std::string_view contentType = http::ContentTypeApplicationOctetStream) & {
    setBodyInternal(std::string_view{});
    setContentTypeHeader(contentType, size == 0);
    _payloadVariant = HttpPayload(std::move(body), size);
    _payloadKind = PayloadKind::Captured;
    return *this;
  }

  // Capture the body by value to avoid a copy (and possibly an allocation).
  // Empty body is allowed.
  // The body instance is moved into this HttpResponse.
  HttpResponse&& body(std::unique_ptr<std::byte[]> body, std::size_t size,
                      std::string_view contentType = http::ContentTypeApplicationOctetStream) && {
    setBodyInternal(std::string_view{});
    setContentTypeHeader(contentType, size == 0);
    _payloadVariant = HttpPayload(std::move(body), size);
    _payloadKind = PayloadKind::Captured;
    return std::move(*this);
  }

  // Stream the contents of an already-open file as the response body.
  // This methods takes ownership of the 'file' object into the response and sends the entire file.
  // Notes:
  //   - file should be opened (`file` must be true)
  //   - Trailers are NOT permitted when using file
  //   - Transfer coding: file produces a fixed-length response (Content-Length is set) and disables chunked
  //     transfer encoding. For HEAD requests the Content-Length header will be present but the body is suppressed.
  //   - Errors: filesystem read/write errors are surfaced during transmission; callers should expect the connection
  //     to be closed on fatal I/O failures.
  //   - Content Type header: if non-empty, sets given content type value. Otherwise, attempt to guess it from the file
  //     object. If the MIME type is unknown, sets 'application/octet-stream' as Content type.
  HttpResponse& file(File fileObj, std::string_view contentType = {}) & {
    return file(std::move(fileObj), 0, 0, contentType);
  }

  // Stream the contents of an already-open file as the response body.
  // This methods takes ownership of the 'file' object into the response and sends the entire file.
  // Notes:
  //   - file should be opened (`file` must be true)
  //   - Trailers are NOT permitted when using file
  //   - Transfer coding: file produces a fixed-length response (Content-Length is set) and disables chunked
  //     transfer encoding. For HEAD requests the Content-Length header will be present but the body is suppressed.
  //   - Errors: filesystem read/write errors are surfaced during transmission; callers should expect the connection
  //     to be closed on fatal I/O failures.
  //   - Content Type header: if non-empty, sets given content type value. Otherwise, attempt to guess it from the file
  //     object. If the MIME type is unknown, sets 'application/octet-stream' as Content type.
  HttpResponse&& file(File fileObj, std::string_view contentType = {}) && {
    file(std::move(fileObj), 0, 0, contentType);
    return std::move(*this);
  }

  // Stream the contents of an already-open file as the response body.
  // This methods takes ownership of the 'file' object into the response and sends the [offset, offset+length) range.
  // Notes:
  //   - file should be opened (`file` must be true)
  //   - Trailers are NOT permitted when using file
  //   - Transfer coding: file produces a fixed-length response (Content-Length is set) and disables chunked
  //     transfer encoding. For HEAD requests the Content-Length header will be present but the body is suppressed.
  //   - Errors: filesystem read/write errors are surfaced during transmission; callers should expect the connection
  //     to be closed on fatal I/O failures.
  //   - Content Type header: if non-empty, sets given content type value. Otherwise, attempt to guess it from the file
  //     object. If the MIME type is unknown, sets 'application/octet-stream' as Content type.
  HttpResponse& file(File fileObj, std::size_t offset, std::size_t length, std::string_view contentType = {}) &;

  // Stream the contents of an already-open file as the response body.
  // This methods takes ownership of the 'file' object into the response and sends the [offset, offset+length) range.
  // Notes:
  //   - file should be opened (`file` must be true)
  //   - Trailers are NOT permitted when using file
  //   - Transfer coding: file produces a fixed-length response (Content-Length is set) and disables chunked
  //     transfer encoding. For HEAD requests the Content-Length header will be present but the body is suppressed.
  //   - Errors: filesystem read/write errors are surfaced during transmission; callers should expect the connection
  //     to be closed on fatal I/O failures.
  //   - Content Type header: if non-empty, sets given content type value. Otherwise, attempt to guess it from the file
  //     object. If the MIME type is unknown, sets 'application/octet-stream' as Content type.
  HttpResponse&& file(File fileObj, std::size_t offset, std::size_t length, std::string_view contentType = {}) && {
    file(std::move(fileObj), offset, length, contentType);
    return std::move(*this);
  }

  // Get the current file stored in this HttpResponse, or nullptr if no file is set.
  [[nodiscard]] const File* file() const noexcept;

  // Check if this HttpResponse has a file payload.
  [[nodiscard]] bool hasFile() const noexcept { return _payloadKind == PayloadKind::File; }

  // Get a view of the current body stored in this HttpResponse.
  // If the body is not present, it returns an empty view.
  [[nodiscard]] std::string_view body() const noexcept;

  // Inserts or replaces the Location header.
  // If the data to be inserted references internal instance memory, the behavior is undefined.
  HttpResponse& location(std::string_view src) & { return header(http::Location, src); }

  // Inserts or replaces the Location header.
  // If the data to be inserted references internal instance memory, the behavior is undefined.
  HttpResponse&& location(std::string_view src) && {
    header(http::Location, src);
    return std::move(*this);
  }

  // Inserts or replaces the Content-Encoding header.
  // If the data to be inserted references internal instance memory, the behavior is undefined.
  HttpResponse& contentEncoding(std::string_view src) & { return header(http::ContentEncoding, src); }

  // Inserts or replaces the Content-Encoding header.
  // If the data to be inserted references internal instance memory, the behavior is undefined.
  HttpResponse&& contentEncoding(std::string_view src) && {
    header(http::ContentEncoding, src);
    return std::move(*this);
  }

  // Append a header line (duplicates allowed, fastest path).
  // No scan over existing headers. Prefer this when duplicates are OK or
  // when constructing headers once.
  // Do not insert any reserved header (for which IsReservedResponseHeader is true), doing so is undefined behavior.
  // If the data to be inserted references internal instance memory, the behavior is undefined.
  HttpResponse& addHeader(std::string_view key, std::string_view value) & {
    assert(!http::IsReservedResponseHeader(key));
    appendHeaderUnchecked(key, value);
    return *this;
  }

  // Convenient overload adding a header to a numeric value.
  HttpResponse& addHeader(std::string_view key, std::integral auto value) & {
    assert(!http::IsReservedResponseHeader(key));
    appendHeaderUnchecked(key, std::string_view(IntegralToCharVector(value)));
    return *this;
  }

  // Append a header line (duplicates allowed, fastest path).
  // No scan over existing headers. Prefer this when duplicates are OK or
  // when constructing headers once.
  // Do not insert any reserved header (for which IsReservedResponseHeader is true), doing so is undefined behavior.
  // If the data to be inserted references internal instance memory, the behavior is undefined.
  HttpResponse&& addHeader(std::string_view key, std::string_view value) && {
    assert(!http::IsReservedResponseHeader(key));
    appendHeaderUnchecked(key, value);
    return std::move(*this);
  }

  // Convenient overload adding a header to a numeric value.
  HttpResponse&& addHeader(std::string_view key, std::integral auto value) && {
    assert(!http::IsReservedResponseHeader(key));
    appendHeaderUnchecked(key, std::string_view(IntegralToCharVector(value)));
    return std::move(*this);
  }

  // Set or replace a header value ensuring at most one instance.
  // Performs a linear scan (slower than addHeader()) using case-insensitive comparison of header names per
  // RFC 7230 (HTTP field names are case-insensitive). The original casing of the first occurrence is preserved.
  // If not found, falls back to addHeader(). Use only when you must guarantee uniqueness; otherwise prefer
  // addHeader().
  // Do not insert any reserved header (for which IsReservedResponseHeader is true), doing so is undefined behavior.
  // If the data to be inserted references internal instance memory, the behavior is undefined.
  HttpResponse& header(std::string_view key, std::string_view value) & {
    assert(!http::IsReservedResponseHeader(key));
    setHeader(key, value, false);
    return *this;
  }

  // Convenient overload setting a header to a numeric value.
  HttpResponse& header(std::string_view key, std::integral auto value) & {
    assert(!http::IsReservedResponseHeader(key));
    setHeader(key, std::string_view(IntegralToCharVector(value)), false);
    return *this;
  }

  // Set or replace a header value ensuring at most one instance.
  // Performs a linear scan (slower than addHeader()) using case-insensitive comparison of header names per
  // RFC 7230 (HTTP field names are case-insensitive). The original casing of the first occurrence is preserved.
  // If not found, falls back to addHeader(). Use only when you must guarantee uniqueness; otherwise prefer
  // addHeader().
  // Do not insert any reserved header (for which IsReservedResponseHeader is true), doing so is undefined behavior.
  // If the data to be inserted references internal instance memory, the behavior is undefined.
  HttpResponse&& header(std::string_view key, std::string_view value) && {
    assert(!http::IsReservedResponseHeader(key));
    setHeader(key, value, false);
    return std::move(*this);
  }

  // Convenient overload setting a header to a numeric value.
  HttpResponse&& header(std::string_view key, std::integral auto value) && {
    assert(!http::IsReservedResponseHeader(key));
    setHeader(key, std::string_view(IntegralToCharVector(value)), false);
    return std::move(*this);
  }

  // Retrieves the value of the first occurrence of the given header key (case-insensitive search per RFC 7230).
  // If the header is not found, returns an empty string_view.
  // To distinguish between missing and present-but-empty header values, use headerValue().
  [[nodiscard]] std::string_view headerValueOrEmpty(std::string_view key) const noexcept {
    if (const auto optValue = headerValue(key); optValue) {
      return *optValue;
    }
    return {};
  }

  // Retrieves the value of the first occurrence of the given header key (case-insensitive search per RFC 7230).
  // If the header is not found, returns std::nullopt.
  [[nodiscard]] std::optional<std::string_view> headerValue(std::string_view key) const noexcept;

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
  HttpResponse& addTrailer(std::string_view name, std::string_view value) & {
    appendTrailer(name, value);
    return *this;
  }

  HttpResponse&& addTrailer(std::string_view name, std::string_view value) && {
    appendTrailer(name, value);
    return std::move(*this);
  }

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
    if (const FilePayload* pFilePayload = filePayloadPtr(); pFilePayload != nullptr) {
      return static_cast<std::size_t>(pFilePayload->length);
    }
    if (_trailerPos != 0) {
      return _trailerPos;
    }
    const HttpPayload* pExternPayload = externPayloadPtr();
    return pExternPayload != nullptr ? pExternPayload->size() : internalBodyAndTrailersLen();
  }

  [[nodiscard]] std::size_t internalBodyAndTrailersLen() const noexcept { return _data.size() - _bodyStartPos; }

  void setStatusCode(http::StatusCode statusCode) noexcept {
    assert(statusCode >= 100 && statusCode < 1000);
    write3(_data.data() + kStatusCodeBeg, statusCode);
  }

  void setReason(std::string_view newReason);

  void setBodyInternal(std::string_view newBody);

  void setHeader(std::string_view key, std::string_view value, bool onlyIfNew = false);

  void setContentTypeHeader(std::string_view contentTypeValue, bool isEmpty);

  void removeContentTypeHeader();

  void appendHeaderUnchecked(std::string_view key, std::string_view value);

  void appendDateUnchecked(SysTimePoint tp);

  void appendTrailer(std::string_view name, std::string_view value);

  struct PreparedResponse {
    HttpResponseData data;
    File file;
    std::uint64_t fileOffset{0};
    std::uint64_t fileLength{0};
  };

  struct FilePayload {
    File file;
    std::size_t offset{0};
    std::size_t length{0};
  };

  // IMPORTANT: This method finalizes the response by appending reserved headers,
  // and returns the internal buffers stolen from this HttpResponse instance.
  // So this instance must not be used anymore after this call.
  PreparedResponse finalizeAndStealData(http::Version version, SysTimePoint tp, bool keepAlive,
                                        std::span<const http::Header> globalHeaders, bool isHeadMethod,
                                        std::size_t minCapturedBodySize);

  HttpPayload* externPayloadPtr() noexcept { return std::get_if<HttpPayload>(&_payloadVariant); }
  [[nodiscard]] const HttpPayload* externPayloadPtr() const noexcept {
    return std::get_if<HttpPayload>(&_payloadVariant);
  }

  FilePayload* filePayloadPtr() noexcept { return std::get_if<FilePayload>(&_payloadVariant); }
  [[nodiscard]] const FilePayload* filePayloadPtr() const noexcept {
    return std::get_if<FilePayload>(&_payloadVariant);
  }

  RawChars _data;
  uint16_t _headersStartPos{0};  // position just at the CRLF that starts the first header line
  bool _userProvidedContentEncoding{false};
  PayloadKind _payloadKind{PayloadKind::Inline};
  uint32_t _bodyStartPos{0};  // position of first body byte (after CRLF CRLF)
  // Variant holding either an external captured payload (HttpPayload) or a FilePayload.
  // monostate represents "no external payload".
  std::variant<std::monostate, HttpPayload, FilePayload> _payloadVariant;
  std::size_t _trailerPos{0};  // trailer pos in relative to body start
};

}  // namespace aeronet