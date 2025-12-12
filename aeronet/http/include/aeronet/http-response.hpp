#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "aeronet/concatenated-headers.hpp"
#include "aeronet/file.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-payload.hpp"
#include "aeronet/http-response-data.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/http-version.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/simple-charconv.hpp"
#include "aeronet/stringconv.hpp"
#include "aeronet/timedef.hpp"

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

  // Constructs an HttpResponse with a 200 status code, empty reason phrase and given body.
  // The body is copied into the internal buffer, and the content type header is set if the body is not empty.
  // If the body is large, prefer the capture by value of body() overloads to avoid a copy (and possibly an allocation).
  // The content type defaults to "text/plain"
  explicit HttpResponse(std::string_view body, std::string_view contentType = http::ContentTypeTextPlain);

  // Get the current status code stored in this HttpResponse.
  [[nodiscard]] http::StatusCode status() const noexcept {
    return static_cast<http::StatusCode>(read3(_data.data() + kStatusCodeBeg));
  }

  // Get the current reason stored in this HttpResponse.
  [[nodiscard]] std::string_view reason() const noexcept { return {_data.data() + kReasonBeg, reasonLen()}; }

  // Retrieves the value of the first occurrence of the given header key (case-insensitive search per RFC 7230).
  // If the header is not found, returns std::nullopt.
  [[nodiscard]] std::optional<std::string_view> headerValue(std::string_view key) const noexcept;

  // Retrieves the value of the first occurrence of the given header key (case-insensitive search per RFC 7230).
  // If the header is not found, returns an empty string_view.
  // To distinguish between missing and present-but-empty header values, use headerValue().
  [[nodiscard]] std::string_view headerValueOrEmpty(std::string_view key) const noexcept {
    const auto optValue = headerValue(key);
    return optValue ? *optValue : std::string_view{};
  }

  // Get a view of the current body stored in this HttpResponse.
  // If the body is not present, it returns an empty view.
  [[nodiscard]] std::string_view body() const noexcept;

  // Get the current file stored in this HttpResponse, or nullptr if no file is set.
  [[nodiscard]] const File* file() const noexcept;

  // Check if this HttpResponse has a file payload.
  [[nodiscard]] bool hasFile() const noexcept { return isFileBody(); }

  // Get a view of the current trailers stored in this HttpResponse, starting at the first
  // trailer key (if any).
  // Each trailer line is formatted as: name + ": " + value + CRLF.
  // If no trailers are present, it returns an empty view.
  [[nodiscard]] std::string_view trailers() const noexcept {
    const auto* pExternPayload = externPayloadPtr();
    return pExternPayload != nullptr ? externalTrailers(*pExternPayload) : internalTrailers();
  }

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

  // Append a value to an existing header, inserting the header if it is currently missing.
  // The existing header value is expanded in-place by inserting `separator` followed by `value`.
  // If the header does not exist yet this behaves like addHeader(key, value).
  // Do not insert any reserved header (for which IsReservedResponseHeader is true), doing so is undefined behavior.
  // If the data to be inserted references internal instance memory, the behavior is undefined.
  HttpResponse& appendHeaderValue(std::string_view key, std::string_view value, std::string_view separator = ", ") & {
    appendHeaderValueInternal(key, value, separator);
    return *this;
  }

  // Convenient overload appending a numeric value.
  HttpResponse& appendHeaderValue(std::string_view key, std::integral auto value, std::string_view separator = ", ") & {
    appendHeaderValueInternal(key, std::string_view(IntegralToCharVector(value)), separator);
    return *this;
  }

  // Append a value to an existing header, inserting the header if it is currently missing.
  HttpResponse&& appendHeaderValue(std::string_view key, std::string_view value, std::string_view separator = ", ") && {
    appendHeaderValueInternal(key, value, separator);
    return std::move(*this);
  }

  // Convenient overload appending a numeric value.
  HttpResponse&& appendHeaderValue(std::string_view key, std::integral auto value,
                                   std::string_view separator = ", ") && {
    appendHeaderValueInternal(key, std::string_view(IntegralToCharVector(value)), separator);
    return std::move(*this);
  }

  // Append a header line (duplicates allowed, fastest path).
  // No scan over existing headers. Prefer this when duplicates are OK or when constructing headers once.
  // Do not insert any reserved header (for which IsReservedResponseHeader is true), doing so is undefined behavior.
  // If the data to be inserted references internal instance memory, the behavior is undefined.
  HttpResponse& addHeader(std::string_view key, std::string_view value) & {
    appendHeaderInternal(key, value);
    return *this;
  }

  // Convenient overload adding a header whose value is numeric.
  HttpResponse& addHeader(std::string_view key, std::integral auto value) & {
    appendHeaderInternal(key, std::string_view(IntegralToCharVector(value)));
    return *this;
  }

  // Append a header line (duplicates allowed, fastest path).
  // No scan over existing headers. Prefer this when duplicates are OK or when constructing headers once.
  // Do not insert any reserved header (for which IsReservedResponseHeader is true), doing so is undefined behavior.
  // If the data to be inserted references internal instance memory, the behavior is undefined.
  HttpResponse&& addHeader(std::string_view key, std::string_view value) && {
    appendHeaderInternal(key, value);
    return std::move(*this);
  }

  // Convenient overload adding a header whose value is numeric.
  HttpResponse&& addHeader(std::string_view key, std::integral auto value) && {
    appendHeaderInternal(key, std::string_view(IntegralToCharVector(value)));
    return std::move(*this);
  }

  // Add or replace a header value entirely ensuring at most one instance.
  // Performs a linear scan (slower than addHeader()) using case-insensitive comparison of header names per
  // RFC 7230 (HTTP field names are case-insensitive). The original casing of the first occurrence is preserved.
  // Do not insert any reserved header (for which IsReservedResponseHeader is true), doing so is undefined behavior.
  // If the data to be inserted references internal instance memory, the behavior is undefined.
  HttpResponse& header(std::string_view key, std::string_view value) & {
    setHeader(key, value, OnlyIfNew::No);
    return *this;
  }

  // Convenient overload setting a header to a numeric value.
  HttpResponse& header(std::string_view key, std::integral auto value) & {
    setHeader(key, std::string_view(IntegralToCharVector(value)), OnlyIfNew::No);
    return *this;
  }

  // Add or replace a header value entirely ensuring at most one instance.
  // Performs a linear scan (slower than addHeader()) using case-insensitive comparison of header names per
  // RFC 7230 (HTTP field names are case-insensitive). The original casing of the first occurrence is preserved.
  // Do not insert any reserved header (for which IsReservedResponseHeader is true), doing so is undefined behavior.
  // If the data to be inserted references internal instance memory, the behavior is undefined.
  HttpResponse&& header(std::string_view key, std::string_view value) && {
    setHeader(key, value, OnlyIfNew::No);
    return std::move(*this);
  }

  // Convenient overload setting a header to a numeric value.
  HttpResponse&& header(std::string_view key, std::integral auto value) && {
    setHeader(key, std::string_view(IntegralToCharVector(value)), OnlyIfNew::No);
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
    setCapturedPayload(std::move(body));
    return *this;
  }

  // Capture the body by value to avoid a copy (and possibly an allocation).
  // Empty body is allowed.
  // The body instance is moved into this HttpResponse.
  HttpResponse&& body(std::string body, std::string_view contentType = http::ContentTypeTextPlain) && {
    setBodyInternal(std::string_view{});
    setContentTypeHeader(contentType, body.empty());
    setCapturedPayload(std::move(body));
    return std::move(*this);
  }

  // Capture the body by value to avoid a copy (and possibly an allocation).
  // Empty body is allowed.
  // The body instance is moved into this HttpResponse.
  HttpResponse& body(std::vector<char> body, std::string_view contentType = http::ContentTypeApplicationOctetStream) & {
    setBodyInternal(std::string_view{});
    setContentTypeHeader(contentType, body.empty());
    setCapturedPayload(std::move(body));
    return *this;
  }

  // Capture the body by value to avoid a copy (and possibly an allocation).
  // Empty body is allowed.
  // The body instance is moved into this HttpResponse.
  HttpResponse&& body(std::vector<std::byte> body,
                      std::string_view contentType = http::ContentTypeApplicationOctetStream) && {
    setBodyInternal(std::string_view{});
    setContentTypeHeader(contentType, body.empty());
    setCapturedPayload(std::move(body));
    return std::move(*this);
  }

  // Capture the body by value to avoid a copy (and possibly an allocation).
  // Empty body is allowed.
  // The body instance is moved into this HttpResponse.
  HttpResponse& body(std::vector<std::byte> body,
                     std::string_view contentType = http::ContentTypeApplicationOctetStream) & {
    setBodyInternal(std::string_view{});
    setContentTypeHeader(contentType, body.empty());
    setCapturedPayload(std::move(body));
    return *this;
  }

  // Capture the body by value to avoid a copy (and possibly an allocation).
  // Empty body is allowed.
  // The body instance is moved into this HttpResponse.
  HttpResponse&& body(std::vector<char> body,
                      std::string_view contentType = http::ContentTypeApplicationOctetStream) && {
    setBodyInternal(std::string_view{});
    setContentTypeHeader(contentType, body.empty());
    setCapturedPayload(std::move(body));
    return std::move(*this);
  }

  // Capture the body by value to avoid a copy (and possibly an allocation).
  // Empty body is allowed.
  // The body instance is moved into this HttpResponse.
  HttpResponse& body(std::unique_ptr<char[]> body, std::size_t size,
                     std::string_view contentType = http::ContentTypeApplicationOctetStream) & {
    setBodyInternal(std::string_view{});
    setContentTypeHeader(contentType, size == 0);
    setCapturedPayload(std::move(body), size);
    return *this;
  }

  // Capture the body by value to avoid a copy (and possibly an allocation).
  // Empty body is allowed.
  // The body instance is moved into this HttpResponse.
  HttpResponse&& body(std::unique_ptr<char[]> body, std::size_t size,
                      std::string_view contentType = http::ContentTypeApplicationOctetStream) && {
    setBodyInternal(std::string_view{});
    setContentTypeHeader(contentType, size == 0);
    setCapturedPayload(std::move(body), size);
    return std::move(*this);
  }

  // Capture the body by value to avoid a copy (and possibly an allocation).
  // Empty body is allowed.
  // The body instance is moved into this HttpResponse.
  HttpResponse& body(std::unique_ptr<std::byte[]> body, std::size_t size,
                     std::string_view contentType = http::ContentTypeApplicationOctetStream) & {
    setBodyInternal(std::string_view{});
    setContentTypeHeader(contentType, size == 0);
    setCapturedPayload(std::move(body), size);
    return *this;
  }

  // Capture the body by value to avoid a copy (and possibly an allocation).
  // Empty body is allowed.
  // The body instance is moved into this HttpResponse.
  HttpResponse&& body(std::unique_ptr<std::byte[]> body, std::size_t size,
                      std::string_view contentType = http::ContentTypeApplicationOctetStream) && {
    setBodyInternal(std::string_view{});
    setContentTypeHeader(contentType, size == 0);
    setCapturedPayload(std::move(body), size);
    return std::move(*this);
  }

  // Append an inline body from a `std::string_view`.
  // - `body` is copied into the internal inline buffer (no ownership transferred).
  // - If `body` is empty this call appends nothing and does NOT clear any existing inline body.
  //   To clear the body explicitly use `body("")` or one of the `body(...)` overloads with an empty value.
  // - `contentType` is optional. If non-empty it replaces the current Content-Type header.
  //   If empty and no Content-Type header exists yet, the header is set to
  //   `application/octet-stream` only when the appended data is non-empty.
  // - Safe to call multiple times; data is appended to any existing inline body.
  // Trailers should not be added before calling this method.
  HttpResponse& appendBody(std::string_view body, std::string_view contentType = {}) & {
    appendBodyInternal(body, contentType);
    return *this;
  }

  // Rvalue overload: same semantics as the lvalue overload but returns an rvalue reference
  // to allow fluent chaining on temporaries.
  HttpResponse&& appendBody(std::string_view body, std::string_view contentType = {}) && {
    appendBodyInternal(body, contentType);
    return std::move(*this);
  }

  // Append an inline body from a span of bytes.
  // - `body` contents are copied into the internal inline buffer.
  // - When `body` is non-empty and `contentType` is empty, the default
  //   `application/octet-stream` content type will be set.
  // - This overload is convenient when working with raw byte buffers.
  // Trailers should not be added before calling this method.
  HttpResponse& appendBody(std::span<const std::byte> body, std::string_view contentType = {}) & {
    if (!body.empty() && contentType.empty()) {
      contentType = http::ContentTypeApplicationOctetStream;
    }
    appendBodyInternal(std::string_view{reinterpret_cast<const char*>(body.data()), body.size()}, contentType);
    return *this;
  }

  // Rvalue overload for span-based append.
  HttpResponse&& appendBody(std::span<const std::byte> body, std::string_view contentType = {}) && {
    if (!body.empty() && contentType.empty()) {
      contentType = http::ContentTypeApplicationOctetStream;
    }
    appendBodyInternal(std::string_view{reinterpret_cast<const char*>(body.data()), body.size()}, contentType);
    return std::move(*this);
  }

  // Append an inline body from a NUL-terminated C string (or nullptr for empty body).
  // - If `body` is `nullptr` it is treated as an empty body.
  // - Otherwise the C-string is copied into the inline buffer.
  // - `contentType` follows the same rules as the `std::string_view` overload.
  // Trailers should not be added before calling this method.
  HttpResponse& appendBody(const char* body, std::string_view contentType = {}) & {
    auto sv = body == nullptr ? std::string_view() : std::string_view(body);
    appendBodyInternal(sv, contentType);
    return *this;
  }

  // Rvalue overload for C-string append.
  HttpResponse&& appendBody(const char* body, std::string_view contentType = {}) && {
    auto sv = body == nullptr ? std::string_view() : std::string_view(body);
    appendBodyInternal(sv, contentType);
    return std::move(*this);
  }

  // Appends directly inside the body up to 'maxLen' bytes of data.
  // 'writer' should return the actual number of bytes written (should be <= 'maxLen'),
  // and provides as a single argument the start of the buffer where to append body data.
  // If body was previously captured, it is erased and append with initiate a new inline body.
  // It is undefined behavior to write more than 'maxLen' bytes of data into the writer (for one call).
  // This is the most efficient way to set the body to the HttpResponse, as it avoids copies and limits allocations.
  // Growing of the internal buffer is exponential.
  // You can call appendBody several times (it will append data to existing inline body).
  // To erase the body, call 'body' with an empty buffer.
  // ContentType is optional - if non-empty, it replaces current body content type.
  // Otherwise, initializes content type to 'application/octet-stream' if content tpye is not already set.
  HttpResponse& appendBody(std::size_t maxLen, const std::function<std::size_t(std::byte*)>& writer,
                           std::string_view contentType = {}) & {
    appendBodyInternal(maxLen, writer, contentType);
    return *this;
  }

  // Convenience overload accepting a writer that writes into a `char*` buffer.
  // Mimicks the behavior of the `std::byte*` based overload, except that the default content type is
  // 'text/plain' if the content type is not already set.
  HttpResponse& appendBody(std::size_t maxLen, const std::function<std::size_t(char*)>& writer,
                           std::string_view contentType = {}) & {
    appendBodyInternal(maxLen, writer, contentType);
    return *this;
  }

  // Appends directly inside the body up to 'maxLen' bytes of data.
  // 'writer' should return the actual number of bytes written (should be <= 'maxLen'),
  // and provides as a single argument the start of the buffer where to append body data.
  // If body was previously captured, it is erased and append with initiate a new inline body.
  // It is undefined behavior to write more than 'maxLen' bytes of data into the writer (for one call).
  // This is the most efficient way to set the body to the HttpResponse, as it avoids copies and limits allocations.
  // Growing of the internal buffer is exponential.
  // You can call appendBody several times (it will append data to existing inline body).
  // To erase the body, call 'body' with an empty buffer.
  // ContentType is optional - if non-empty, it replaces current body content type.
  // Otherwise, initializes content type to 'application/octet-stream' if content tpye is not already set.
  HttpResponse&& appendBody(std::size_t maxLen, const std::function<std::size_t(std::byte*)>& writer,
                            std::string_view contentType = {}) && {
    appendBodyInternal(maxLen, writer, contentType);
    return std::move(*this);
  }

  // Rvalue overload that accepts a `char*` writer.
  // Mimicks the behavior of the `std::byte*` based overload, except that the default content type is
  // 'text/plain' if the content type is not already set.
  HttpResponse&& appendBody(std::size_t maxLen, const std::function<std::size_t(char*)>& writer,
                            std::string_view contentType = {}) && {
    appendBodyInternal(maxLen, writer, contentType);
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
  //   - Content Type header: if non-empty, sets given content type value. Otherwise, attempt to guess it from the
  //   file
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
  HttpResponse& addTrailer(std::string_view name, std::string_view value) & {
    appendTrailer(name, value);
    return *this;
  }

  // Adds a trailer header to be sent after the response body (RFC 7230 §4.1.2).
  HttpResponse&& addTrailer(std::string_view name, std::string_view value) && {
    appendTrailer(name, value);
    return std::move(*this);
  }

 private:
  friend class SingleHttpServer;
  friend class HttpResponseTest;
  friend class HttpResponseWriter;  // streaming writer needs access to finalize

  void setCapturedPayload(auto payload) {
    if (payload.empty()) {
      _payloadVariant = {};
    } else {
      _payloadVariant = HttpPayload(std::move(payload));
    }
  }

  void setCapturedPayload(auto payload, std::size_t size) {
    if (size == 0) {
      _payloadVariant = {};
    } else {
      _payloadVariant = HttpPayload(std::move(payload), size);
    }
  }

  [[nodiscard]] std::size_t reasonLen() const noexcept;

  [[nodiscard]] std::string_view internalTrailers() const noexcept;

  [[nodiscard]] std::string_view externalTrailers(const HttpPayload& data) const noexcept;

  [[nodiscard]] std::size_t bodyLen() const noexcept;

  [[nodiscard]] std::size_t internalBodyAndTrailersLen() const noexcept { return _data.size() - _bodyStartPos; }

  void setStatusCode(http::StatusCode statusCode) noexcept;

  void setReason(std::string_view newReason);

  void setBodyInternal(std::string_view newBody);

  enum class OnlyIfNew : std::uint8_t { No, Yes };

  void setHeader(std::string_view key, std::string_view value, OnlyIfNew onlyIfNew = OnlyIfNew::No);

  void setContentTypeHeader(std::string_view contentTypeValue, bool isEmpty);

  void eraseHeader(std::string_view key);

  void appendHeaderInternal(std::string_view key, std::string_view value);

  void appendHeaderValueInternal(std::string_view key, std::string_view value, std::string_view separator);

  void appendBodyInternal(std::string_view data, std::string_view contentType);

  void appendBodyInternal(std::size_t maxLen, const std::function<std::size_t(char*)>& writer,
                          std::string_view contentType);

  void appendBodyInternal(std::size_t maxLen, const std::function<std::size_t(std::byte*)>& writer,
                          std::string_view contentType);

  HttpPayload* finalizeHeadersBody(http::Version version, SysTimePoint tp, bool isHeadMethod, bool close,
                                   const ConcatenatedHeaders& globalHeaders, std::size_t minCapturedBodySize);

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
  PreparedResponse finalizeAndStealData(http::Version version, SysTimePoint tp, bool close,
                                        const ConcatenatedHeaders& globalHeaders, bool isHeadMethod,
                                        std::size_t minCapturedBodySize);

  HttpPayload* externPayloadPtr() noexcept { return std::get_if<HttpPayload>(&_payloadVariant); }

  [[nodiscard]] const HttpPayload* externPayloadPtr() const noexcept {
    return std::get_if<HttpPayload>(&_payloadVariant);
  }

  FilePayload* filePayloadPtr() noexcept { return std::get_if<FilePayload>(&_payloadVariant); }

  [[nodiscard]] const FilePayload* filePayloadPtr() const noexcept {
    return std::get_if<FilePayload>(&_payloadVariant);
  }

  [[nodiscard]] bool isInlineBody() const noexcept { return _payloadVariant.index() == 0; }
  [[nodiscard]] bool isFileBody() const noexcept { return _payloadVariant.index() == 2; }

  void setBodyEnsureNoTrailers() const {
    if (_trailerPos != 0) {
      throw std::logic_error("Cannot set body after the first trailer");
    }
  }

  void appendBodyResetContentType(std::string_view givenContentType, std::string_view defaultContentType);

  RawChars _data;
  uint16_t _headersStartPos{0};  // position just at the CRLF that starts the first header line
  uint32_t _bodyStartPos{0};     // position of first body byte (after CRLF CRLF)
  // Variant holding either an external captured payload (HttpPayload) or a FilePayload.
  // monostate represents "no external payload".
  std::variant<std::monostate, HttpPayload, FilePayload> _payloadVariant;
  std::size_t _trailerPos{0};  // trailer pos in relative to body start
};

}  // namespace aeronet