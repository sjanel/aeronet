#pragma once

#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "aeronet/concatenated-headers.hpp"
#include "aeronet/file.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-headers-view.hpp"
#include "aeronet/http-payload.hpp"
#include "aeronet/http-response-data.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/http-version.hpp"
#include "aeronet/nchars.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/simple-charconv.hpp"
#include "aeronet/string-trim.hpp"
#include "aeronet/stringconv.hpp"
#include "aeronet/timedef.hpp"

namespace aeronet {

namespace internal {
class HttpCodec;
}  // namespace internal

#ifdef AERONET_ENABLE_HTTP2
namespace http2 {
class Http2ProtocolHandler;
}  // namespace http2
#endif

// -----------------------------------------------------------------------------
// HttpResponse
// -----------------------------------------------------------------------------
// A contiguous single-buffer HTTP/1.x friendly response builder focused on minimal
// allocations and cache-friendly writes, optionally supporting large bodies captured
// in the response. It is also used as the basis for HTTP/2 response serialization,
// so that the API is common between HTTP/1.x and HTTP/2 responses.
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
//   headerAddLine():
//     - O(T) memmove of tail where T = size(DoubleCRLF + current body), no scan of
//       existing headers (fast path). Allows duplicates intentionally.
//
//   header():
//     - Linear scan of current header region to find existing key at line starts
//       (recognised by preceding CRLF). If found, value replaced in-place adjusting
//       buffer via single memmove for size delta. If not found, falls back to append.
//     - Because of the scan it is less efficient than headerAddLine(). Prefer
//       headerAddLine() when duplicates are acceptable or order-only semantics matter.
//
// Mutators & Finalization:
//   status(), reason(), body(), headerAddLine(), header() may be called in any
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
//   - headerAddLine(): O(bodyLen) for memmove of tail
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
//   - Prefer headerAddLine() when duplicates are acceptable or order-only semantics matter.
//   - Minimize header mutations after body() to reduce data movement.
//
// Trailers (outbound / response-side):
//   - HttpResponse supports adding trailer headers that will be transmitted after the
//     response body when the response is serialized. Trailers are intended for metadata
//     computed after body generation (checksums, signatures, processing totals, etc.).
//   - Ordering constraint: trailers MUST be added after the body has been set (via
//     any `body()` overload). This requirement enables a zero-allocation implementation
//     where trailer text is appended directly to the existing body buffer.
//   - If the body is captured from an external buffer (zero-copy), trailers are appended to
//     this external buffer, otherwise they are appended to the internal HttpResponse buffer
//   - Streaming responses: `HttpResponseWriter` implements a separate streaming-safe
//     `trailerAddLine()` API which buffers trailer lines during streaming and emits them
//     after the final zero-length chunk (see `HttpResponseWriter` docs).
// -----------------------------------------------------------------------------
class HttpResponse {
 public:
  // "HTTP/x.y". Should be changed if version major / minor exceed 1 digit
  static constexpr std::size_t kHttp1VersionLen = http::HTTP10Sv.size();
  static constexpr std::size_t kStatusCodeBeg = kHttp1VersionLen + 1;  // index of first status code digit
  static constexpr std::size_t kReasonBeg = kStatusCodeBeg + 3 + 1;    // index of first reason phrase character

  // Minimum initial capacity for HttpResponse internal buffer to avoid too-small allocations.
  // The minimal valid HTTP response that will be returned by aeronet is
  // "HTTP/1.1 200\r\nDate: Tue, 07 Jan 2025 12:34:56 GMT\r\n\r\n" (53 bytes).
  static constexpr std::size_t kHttpResponseMinInitialCapacity = 64UL;

  // Returns the size needed to store a header / trailer with given name and value lengths.
  static constexpr std::size_t HeaderSize(std::size_t nameLen, std::size_t valueLen) {
    return http::CRLF.size() + nameLen + http::HeaderSep.size() + valueLen;
  }

  // Returns the size needed to store a body with given length and optional content type header.
  // It takes into account the required headers (Content-Type and Content-Length).
  static constexpr std::size_t BodySize(std::size_t bodyLen,
                                        std::size_t contentTypeLen = http::ContentTypeTextPlain.size()) {
    return bodyLen + HeaderSize(http::ContentType.size(), contentTypeLen) +
           HeaderSize(http::ContentLength.size(), static_cast<std::size_t>(nchars(bodyLen)));
  }

  // -------------/
  // CONSTRUCTORS /
  // -------------/

  // Constructs an HttpResponse with a StatusCode OK (200) and a default initial capacity.
  HttpResponse() : HttpResponse(http::StatusCodeOK) {}

  // Constructs an HttpResponse with the given status code and a default initial capacity.
  explicit HttpResponse(http::StatusCode code) : HttpResponse(kHttpResponseMinInitialCapacity, code) {}

  // Constructs an HttpResponse with the given status code and body, that will be copied into the internal buffer.
  HttpResponse(http::StatusCode code, std::string_view body, std::string_view contentType = http::ContentTypeTextPlain);

  // Constructs an HttpResponse with an additional initial capacity for the internal buffer.
  // The provided capacity will be added to the minimal required size to hold the status line and reserved headers.
  // Give an approximate sum of added reason, headers, body size and trailers to minimize reallocations.
  HttpResponse(std::size_t additionalCapacity, http::StatusCode code);

  // Constructs an HttpResponse with a 200 status code, no reason phrase and given body.
  // The body is copied into the internal buffer, and the content type header is set if the body is not empty.
  // If the body is large, prefer the capture by value of body() overloads to avoid a copy (and possibly an allocation).
  // The content type defaults to "text/plain"
  explicit HttpResponse(std::string_view body, std::string_view contentType = http::ContentTypeTextPlain)
      : HttpResponse(http::StatusCodeOK, body, contentType) {}

  // Same as above, but with a byte span for the body.
  explicit HttpResponse(std::span<const std::byte> body,
                        std::string_view contentType = http::ContentTypeApplicationOctetStream)
      : HttpResponse(std::string_view(reinterpret_cast<const char*>(body.data()), body.size()), contentType) {}

  // Constructs an HttpResponse with the given additional capacity, status code, concatenated headers,
  // body and content type. The body is copied into the internal buffer.
  // The concatenatedHeaders should follow a strict format. Each header key value pair MUST be formatted as:
  //   <HeaderName>http::HeaderSep<HeaderValue>http::CRLF
  // Examples of concatenatedHeaders, for http::HeaderSep = ": " and http::CRLF = "\r\n":
  //   ""
  //   "HeaderName: Value\r\n"
  //   "HeaderName1: Value1\r\nHeaderName2: Value2\r\n"
  // Empty concatenatedHeaders are allowed.
  // It is undefined behavior to provide incorrectly formatted concatenated headers.
  HttpResponse(std::size_t additionalCapacity, http::StatusCode code, std::string_view concatenatedHeaders,
               std::string_view body = {}, std::string_view contentType = http::ContentTypeTextPlain);

  // --------/
  // GETTERS /
  // --------/

  // Get the current status code stored in this HttpResponse.
  [[nodiscard]] http::StatusCode status() const noexcept {
    return static_cast<http::StatusCode>(read3(_data.data() + kStatusCodeBeg));
  }

  // Get the current status code string view stored in this HttpResponse
  [[nodiscard]] std::string_view statusStr() const noexcept { return {_data.data() + kStatusCodeBeg, 3UL}; }

  // Get the size of the status line including CRLF (with HTTP version, status code, reason if any).
  [[nodiscard]] std::size_t statusLineSize() const noexcept { return headersStartPos() + http::CRLF.size(); }

  // Synonym for statusLineSize().
  [[nodiscard]] std::size_t statusLineLength() const noexcept { return statusLineSize(); }

  // Get the current reason stored in this HttpResponse, or an empty string_view if no reason is set.
  [[nodiscard]] std::string_view reason() const noexcept { return {_data.data() + kReasonBeg, reasonLength()}; }

  // Check if a reason phrase is present.
  [[nodiscard]] bool hasReason() const noexcept { return _data[kReasonBeg] != '\n'; }

  // Get the length of the current reason stored in this HttpResponse.
  [[nodiscard]] std::size_t reasonLength() const noexcept {
    return (headersStartPos() - kReasonBeg) * static_cast<std::size_t>(hasReason());
  }

  // Synonym for reasonLength().
  [[nodiscard]] std::size_t reasonSize() const noexcept { return reasonLength(); }

  // Checks if the given header key is present (case-insensitive search per RFC 7230).
  [[nodiscard]] bool hasHeader(std::string_view key) const noexcept { return headerValue(key).has_value(); }

  // Retrieves the value of the first occurrence of the given header key (case-insensitive search per RFC 7230).
  // If the header is not found, returns std::nullopt.
  [[nodiscard]] std::optional<std::string_view> headerValue(std::string_view key) const noexcept;

  // Retrieves the value of the first occurrence of the given header key (case-insensitive search per RFC 7230).
  // If the header is not found, returns an empty string_view.
  // To distinguish between missing and present-but-empty header values, use headerValue().
  [[nodiscard]] std::string_view headerValueOrEmpty(std::string_view key) const noexcept {
    return headerValue(key).value_or(std::string_view{});
  }

  // Get a contiguous view of the current headers stored in this HttpResponse.
  // Each header line is formatted as: name + ": " + value + CRLF.
  // If no headers are present, it returns an empty view.
  [[nodiscard]] std::string_view headersFlatView() const noexcept;

  // Return a non-allocating, iterable view over headers.
  // Each element is a HeaderView with name and value string_views.
  // Usage example:
  //   for (const auto &[name, value] : response.headers()) {
  //       process(name, value);
  //   }
  [[nodiscard]] HeadersView headers() const noexcept { return HeadersView(headersFlatView()); }

  // Get the total size of all headers, counting exactly one CRLF per header line (excluding final CRLF before body).
  [[nodiscard]] std::size_t headersSize() const noexcept {
    return bodyStartPos() - headersStartPos() - http::DoubleCRLF.size();
  }

  // Synonym for headersSize().
  [[nodiscard]] std::size_t headersLength() const noexcept { return headersSize(); }

  // Get the size of the head (status line + headers), excluding body, but with final CRLF before body.
  [[nodiscard]] std::size_t headSize() const noexcept { return bodyStartPos(); }

  // Synonym for headSize().
  [[nodiscard]] std::size_t headLength() const noexcept { return headSize(); }

  // Get a view of the current in memory body (no file) stored in this HttpResponse.
  // The returned view will be empty if there is either no body, or a file body.
  [[nodiscard]] std::string_view bodyInMemory() const noexcept;

  // Get the current file stored in this HttpResponse, or nullptr if no file is set.
  [[nodiscard]] const File* file() const noexcept;

  // Checks if this HttpResponse has a body (either inlined, captured or file).
  [[nodiscard]] bool hasBody() const noexcept { return !_payloadVariant.empty() || hasBodyInlined(); }

  // Checks if this HttpResponse has a body in memory (either internal buffer or captured, but no file).
  [[nodiscard]] bool hasBodyInMemory() const noexcept { return hasBodyCaptured() || hasBodyInlined(); }

  // Checks if this HttpResponse has an inlined body (appended to the main buffer after headers).
  [[nodiscard]] bool hasBodyInlined() const noexcept { return bodyStartPos() < _data.size(); }

  // Check if this HttpResponse has a captured body (no file).
  [[nodiscard]] bool hasBodyCaptured() const noexcept { return _payloadVariant.hasCapturedBody(); }

  // Check if this HttpResponse has a file payload.
  [[nodiscard]] bool hasBodyFile() const noexcept { return _payloadVariant.isFilePayload(); }

  // Get the length of the current body stored in this HttpResponse, if any (including file).
  [[nodiscard]] std::size_t bodyLength() const noexcept;

  // Synonym for bodyLength().
  [[nodiscard]] std::size_t bodySize() const noexcept { return bodyLength(); }

  // Get the length of the current inlined or captured (but no file) body stored in this HttpResponse.
  [[nodiscard]] std::size_t bodyInMemoryLength() const noexcept {
    return hasBodyCaptured() ? (_payloadVariant.size() - _trailerLen) : bodyInlinedLength();
  }

  // Synonym for bodyInMemoryLength().
  [[nodiscard]] std::size_t bodyInMemorySize() const noexcept { return bodyInMemoryLength(); }

  // Total size of the HttpResponse when serialized, excluding file payload size (if any).
  [[nodiscard]] std::size_t sizeInMemory() const noexcept { return _data.size() + _payloadVariant.size(); }

  // Get the current size of the internal buffer.
  [[nodiscard]] std::size_t sizeInlined() const noexcept { return _data.size(); }

  // Get the current capacity of the internal buffer.
  [[nodiscard]] std::size_t capacityInlined() const noexcept { return _data.capacity(); }

  // Get the length of the current inlined body stored in this HttpResponse.
  [[nodiscard]] std::size_t bodyInlinedLength() const noexcept { return _data.size() - bodyStartPos() - _trailerLen; }

  // Synonym for bodyInlinedLength().
  [[nodiscard]] std::size_t bodyInlinedSize() const noexcept { return bodyInlinedLength(); }

  // Retrieves the value of the first occurrence of the given trailer key (case-insensitive search per RFC 7230).
  // If the trailer is not found, returns std::nullopt.
  [[nodiscard]] std::optional<std::string_view> trailerValue(std::string_view key) const noexcept;

  // Checks if the given trailer key is present (case-insensitive search per RFC 7230).
  [[nodiscard]] bool hasTrailer(std::string_view key) const noexcept { return trailerValue(key).has_value(); }

  // Retrieves the value of the first occurrence of the given trailer key (case-insensitive search per RFC 7230).
  // If the trailer is not found, returns an empty string_view.
  // To distinguish between missing and present-but-empty trailer values, use trailerValue().
  [[nodiscard]] std::string_view trailerValueOrEmpty(std::string_view key) const noexcept {
    return trailerValue(key).value_or(std::string_view{});
  }

  // Get a view of the current trailers stored in this HttpResponse, starting at the first
  // trailer key (if any).
  // Each trailer line is formatted as: name + ": " + value + CRLF.
  // If no trailers are present, it returns an empty view.
  [[nodiscard]] std::string_view trailersFlatView() const noexcept {
    return hasBodyCaptured() ? externalTrailers() : internalTrailers();
  }

  // Return a non-allocating, iterable view over trailer headers.
  // Each element is a HeaderView with name and value string_views.
  // Usage example:
  //   for (const auto &[name, value] : response.headers()) {
  //       process(name, value);
  //   }
  [[nodiscard]] HeadersView trailers() const noexcept { return HeadersView(trailersFlatView()); }

  // ---------------/
  // STATUS SETTERS /
  // ---------------/

  // Replaces the status code. Must be a 3 digits integer.
  // Throws std::invalid_argument if the status code is not in the range [100, 999].
  HttpResponse& status(http::StatusCode statusCode) &;

  // Rvalue overload of status(statusCode).
  HttpResponse&& status(http::StatusCode statusCode) && { return std::move(this->status(statusCode)); }

  // ---------------/
  // REASON SETTERS /
  // ---------------/

  // Sets or replace the reason phrase for this instance.
  // Inserting empty reason is allowed - this will remove any existing reason.
  // If the data to be inserted references internal instance memory, the behavior is undefined.
  // Note that in modern HTTP, the reason phrase is optional and often omitted.
  // In HTTP/2, the reason phrase is not transmitted at all.
  HttpResponse& reason(std::string_view reason) &;

  // Rvalue overload of reason(reason).
  HttpResponse&& reason(std::string_view reason) && { return std::move(this->reason(reason)); }

  // ---------------/
  // HEADER SETTERS /
  // ---------------/

  // Inserts or replaces the Location header.
  // If the data to be inserted references internal instance memory, the behavior is undefined.
  HttpResponse& location(std::string_view src) & { return header(http::Location, src); }

  // Inserts or replaces the Location header.
  // If the data to be inserted references internal instance memory, the behavior is undefined.
  HttpResponse&& location(std::string_view src) && { return std::move(header(http::Location, src)); }

  // Inserts or replaces the Content-Encoding header.
  // Manually setting Content-Encoding header will disable automatic compression handling.
  // If you want to compress using codecs supported by aeronet (such as gzip, deflate, br and zstd),
  // it's recommended to not set Content-Encoding header manually and let the library handle compression.
  // If the data to be inserted references internal instance memory, the behavior is undefined.
  HttpResponse& contentEncoding(std::string_view enc) & { return header(http::ContentEncoding, enc); }

  // Inserts or replaces the Content-Encoding header.
  // Manually setting Content-Encoding header will disable automatic compression handling.
  // If you want to compress using codecs supported by aeronet (such as gzip, deflate, br and zstd),
  // it's recommended to not set Content-Encoding header manually and let the library handle compression.
  // If the data to be inserted references internal instance memory, the behavior is undefined.
  HttpResponse&& contentEncoding(std::string_view enc) && { return std::move(header(http::ContentEncoding, enc)); }

  // Append a header line (duplicates allowed, fastest path).
  // No scan over existing headers. Prefer this when duplicates are OK or when constructing headers once.
  // Do not insert any reserved header (for which IsReservedResponseHeader is true), doing so is undefined behavior.
  // If the data to be inserted references internal instance memory, the behavior is undefined.
  HttpResponse& headerAddLine(std::string_view key, std::string_view value) &;

  // Rvalue overload of headerAddLine.
  HttpResponse&& headerAddLine(std::string_view key, std::string_view value) && {
    return std::move(headerAddLine(key, value));
  }

  // Convenient overload adding a header whose value is numeric.
  HttpResponse& headerAddLine(std::string_view key, std::integral auto value) & {
    return headerAddLine(key, std::string_view(IntegralToCharVector(value)));
  }

  // Convenient overload adding a header whose value is numeric.
  HttpResponse&& headerAddLine(std::string_view key, std::integral auto value) && {
    return std::move(headerAddLine(key, std::string_view(IntegralToCharVector(value))));
  }

  // Append a value to an existing header, inserting the header if it is currently missing.
  // The existing header value is expanded in-place by inserting `separator` followed by `value`.
  // If the header does not exist yet this behaves like headerAddLine(key, value).
  // Do not insert any reserved header (for which IsReservedResponseHeader is true), doing so is undefined behavior.
  // If the data to be inserted references internal instance memory, the behavior is undefined.
  HttpResponse& headerAppendValue(std::string_view key, std::string_view value, std::string_view separator = ", ") &;

  // Append a value to an existing header, inserting the header if it is currently missing.
  HttpResponse&& headerAppendValue(std::string_view key, std::string_view value, std::string_view separator = ", ") && {
    return std::move(headerAppendValue(key, value, separator));
  }

  // Convenient overload appending a numeric value.
  HttpResponse& headerAppendValue(std::string_view key, std::integral auto value, std::string_view separator = ", ") & {
    return headerAppendValue(key, std::string_view(IntegralToCharVector(value)), separator);
  }

  // Convenient overload appending a numeric value.
  HttpResponse&& headerAppendValue(std::string_view key, std::integral auto value,
                                   std::string_view separator = ", ") && {
    return std::move(headerAppendValue(key, std::string_view(IntegralToCharVector(value)), separator));
  }

  // Add or replace first header 'key' with 'value'.
  // Performs a linear scan (slower than headerAddLine()) using case-insensitive comparison of header names per
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
  // Performs a linear scan (slower than headerAddLine()) using case-insensitive comparison of header names per
  // RFC 7230 (HTTP field names are case-insensitive). The original casing of the first occurrence is preserved.
  // Do not insert any reserved header (for which IsReservedResponseHeader is true), doing so is undefined behavior.
  // If the data to be inserted references internal instance memory, the behavior is undefined.
  HttpResponse&& header(std::string_view key, std::string_view value) && { return std::move(header(key, value)); }

  // Convenient overload setting a header to a numeric value.
  HttpResponse&& header(std::string_view key, std::integral auto value) && {
    return std::move(header(key, std::string_view(IntegralToCharVector(value))));
  }

  // -------------/
  // BODY SETTERS /
  // -------------/

  // Assigns the given body to this HttpResponse.
  // Empty body is allowed - this will remove any existing body.
  // The whole buffer is copied internally in the HttpResponse. If the body is large, prefer the capture by value of
  // body() overloads to avoid a copy (and possibly an allocation).
  // Body referencing internal memory of this HttpResponse is undefined behavior.
  HttpResponse& body(std::string_view body, std::string_view contentType = http::ContentTypeTextPlain) & {
    setBodyHeaders(contentType, body.size());
    setBodyInternal(body);
    if (isHead()) {
      setHeadSize(body.size());
    } else {
      _payloadVariant = {};
    }
    return *this;
  }

  // Assigns the given body to this HttpResponse.
  // Empty body is allowed - this will remove any existing body.
  // The whole buffer is copied internally in the HttpResponse. If the body is large, prefer the capture by value of
  // body() overloads to avoid a copy (and possibly an allocation).
  // Body referencing internal memory of this HttpResponse is undefined behavior.
  HttpResponse&& body(std::string_view body, std::string_view contentType = http::ContentTypeTextPlain) && {
    setBodyHeaders(contentType, body.size());
    setBodyInternal(body);
    if (isHead()) {
      setHeadSize(body.size());
    } else {
      _payloadVariant = {};
    }
    return std::move(*this);
  }

  // Assigns the given body to this HttpResponse.
  // Empty body is allowed - this will remove any existing body.
  // The whole buffer is copied internally in the HttpResponse. If the body is large, prefer the capture by value of
  // body() overloads to avoid a copy (and possibly an allocation).
  // Body referencing internal memory of this HttpResponse is undefined behavior.
  HttpResponse& body(std::span<const std::byte> body,
                     std::string_view contentType = http::ContentTypeApplicationOctetStream) & {
    return this->body(std::string_view{reinterpret_cast<const char*>(body.data()), body.size()}, contentType);
  }

  // Assigns the given body to this HttpResponse.
  // Empty body is allowed - this will remove any existing body.
  // The whole buffer is copied internally in the HttpResponse. If the body is large, prefer the capture by value of
  // body() overloads to avoid a copy (and possibly an allocation).
  // Body referencing internal memory of this HttpResponse is undefined behavior.
  HttpResponse&& body(std::span<const std::byte> body,
                      std::string_view contentType = http::ContentTypeApplicationOctetStream) && {
    return std::move(
        this->body(std::string_view{reinterpret_cast<const char*>(body.data()), body.size()}, contentType));
  }

  // Assigns the given body to this HttpResponse. The pointer MUST be null-terminated (or nullptr to empty body).
  // Empty body is allowed (Both "" and nullptr will be considered as empty body).
  // The whole buffer is copied internally in the HttpResponse. If the body is large, prefer the capture by value of
  // body() overloads to avoid a copy (and possibly an allocation).
  // Body referencing internal memory of this HttpResponse is undefined behavior.
  HttpResponse& body(const char* body, std::string_view contentType = http::ContentTypeTextPlain) & {
    return this->body(body == nullptr ? std::string_view() : std::string_view(body), contentType);
  }

  // Assigns the given body to this HttpResponse.
  // Empty body is allowed - this will remove any existing body.
  // The whole buffer is copied internally in the HttpResponse. If the body is large, prefer the capture by value of
  // body() overloads to avoid a copy (and possibly an allocation).
  // Body referencing internal memory of this HttpResponse is undefined behavior.
  HttpResponse&& body(const char* body, std::string_view contentType = http::ContentTypeTextPlain) && {
    return std::move(this->body(body, contentType));
  }

  // Capture the body by value to avoid a copy (and possibly an allocation).
  // Empty body is allowed - this will remove any existing body.
  // The body instance is moved into this HttpResponse.
  HttpResponse& body(std::string body, std::string_view contentType = http::ContentTypeTextPlain) & {
    setBodyHeaders(contentType, body.size());
    setBodyInternal(std::string_view{});
    setCapturedPayload(std::move(body));
    return *this;
  }

  // Capture the body by value to avoid a copy (and possibly an allocation).
  // Empty body is allowed - this will remove any existing body.
  // The body instance is moved into this HttpResponse.
  HttpResponse&& body(std::string body, std::string_view contentType = http::ContentTypeTextPlain) && {
    return std::move(this->body(std::move(body), contentType));
  }

  // Capture the body by value to avoid a copy (and possibly an allocation).
  // Empty body is allowed - this will remove any existing body.
  // The body instance is moved into this HttpResponse.
  HttpResponse& body(std::vector<char> body, std::string_view contentType = http::ContentTypeApplicationOctetStream) & {
    setBodyHeaders(contentType, body.size());
    setBodyInternal(std::string_view{});
    setCapturedPayload(std::move(body));
    return *this;
  }

  // Capture the body by value to avoid a copy (and possibly an allocation).
  // Empty body is allowed - this will remove any existing body.
  // The body instance is moved into this HttpResponse.
  HttpResponse&& body(std::vector<std::byte> body,
                      std::string_view contentType = http::ContentTypeApplicationOctetStream) && {
    return std::move(this->body(std::move(body), contentType));
  }

  // Capture the body by value to avoid a copy (and possibly an allocation).
  // Empty body is allowed - this will remove any existing body.
  // The body instance is moved into this HttpResponse.
  HttpResponse& body(std::vector<std::byte> body,
                     std::string_view contentType = http::ContentTypeApplicationOctetStream) & {
    setBodyHeaders(contentType, body.size());
    setBodyInternal(std::string_view{});
    setCapturedPayload(std::move(body));
    return *this;
  }

  // Capture the body by value to avoid a copy (and possibly an allocation).
  // Empty body is allowed - this will remove any existing body.
  // The body instance is moved into this HttpResponse.
  HttpResponse&& body(std::vector<char> body,
                      std::string_view contentType = http::ContentTypeApplicationOctetStream) && {
    return std::move(this->body(std::move(body), contentType));
  }

  // Capture the body by value to avoid a copy (and possibly an allocation).
  // Empty body is allowed - this will remove any existing body.
  // The body instance is moved into this HttpResponse.
  HttpResponse& body(std::unique_ptr<char[]> body, std::size_t size,
                     std::string_view contentType = http::ContentTypeApplicationOctetStream) & {
    setBodyHeaders(contentType, size);
    setBodyInternal(std::string_view{});
    setCapturedPayload(std::move(body), size);
    return *this;
  }

  // Capture the body by value to avoid a copy (and possibly an allocation).
  // Empty body is allowed - this will remove any existing body.
  // The body instance is moved into this HttpResponse.
  HttpResponse&& body(std::unique_ptr<char[]> body, std::size_t size,
                      std::string_view contentType = http::ContentTypeApplicationOctetStream) && {
    return std::move(this->body(std::move(body), size, contentType));
  }

  // Capture the body by value to avoid a copy (and possibly an allocation).
  // Empty body is allowed - this will remove any existing body.
  // The body instance is moved into this HttpResponse.
  HttpResponse& body(std::unique_ptr<std::byte[]> body, std::size_t size,
                     std::string_view contentType = http::ContentTypeApplicationOctetStream) & {
    setBodyHeaders(contentType, size);
    setBodyInternal(std::string_view{});
    setCapturedPayload(std::move(body), size);
    return *this;
  }

  // Capture the body by value to avoid a copy (and possibly an allocation).
  // Empty body is allowed - this will remove any existing body.
  // The body instance is moved into this HttpResponse.
  HttpResponse&& body(std::unique_ptr<std::byte[]> body, std::size_t size,
                      std::string_view contentType = http::ContentTypeApplicationOctetStream) && {
    return std::move(this->body(std::move(body), size, contentType));
  }

  // Sets the body of this HttpResponse to point to a static buffer.
  // No copy is performed, the lifetime of pointed storage must be constant.
  HttpResponse& bodyStatic(std::string_view staticBody, std::string_view contentType = http::ContentTypeTextPlain) & {
    setBodyHeaders(contentType, staticBody.size());
    setBodyInternal(std::string_view{});
    setCapturedPayload(staticBody);
    return *this;
  }

  // Sets the body of this HttpResponse to point to a static buffer.
  // No copy is performed, the lifetime of pointed storage must be constant.
  HttpResponse&& bodyStatic(std::string_view staticBody, std::string_view contentType = http::ContentTypeTextPlain) && {
    return std::move(this->bodyStatic(staticBody, contentType));
  }

  // Same as string_view-based static body, but accepts a span of bytes, and defaults content type to
  // 'application/octet-stream' if not specified.
  HttpResponse& bodyStatic(std::span<const std::byte> staticBody,
                           std::string_view contentType = http::ContentTypeApplicationOctetStream) & {
    return this->bodyStatic(std::string_view{reinterpret_cast<const char*>(staticBody.data()), staticBody.size()},
                            contentType);
  }

  // Rvalue overload for span-based static body.
  HttpResponse&& bodyStatic(std::span<const std::byte> staticBody,
                            std::string_view contentType = http::ContentTypeApplicationOctetStream) && {
    return std::move(this->bodyStatic(staticBody, contentType));
  }

  // Appends data to the body (internal or captured) from a `std::string_view`.
  // Not compatible with captured file bodies, it will throw std::logic_error if the current body is a file.
  // - If `body` is empty this call appends nothing and does NOT clear any existing body.
  //   To clear the body explicitly use `body("")` or one of the `body(...)` overloads with an empty value.
  // - `contentType` is optional. If non-empty it replaces the current Content-Type header.
  //   If empty and no Content-Type header exists yet, the header is set to
  //   `text/plain` only when the appended data is non-empty.
  // - Safe to call multiple times; data is appended to any existing inline body.
  // Trailers should not be added before calling this method.
  HttpResponse& bodyAppend(std::string_view body, std::string_view contentType = {}) &;

  // Rvalue overload: same semantics as the lvalue overload but returns an rvalue reference
  // to allow fluent chaining on temporaries.
  HttpResponse&& bodyAppend(std::string_view body, std::string_view contentType = {}) && {
    return std::move(this->bodyAppend(body, contentType));
  }

  // Same as string_view-based append, but accepts a span of bytes, and defaults content type to
  // 'application/octet-stream' if not specified and body is non-empty.
  HttpResponse& bodyAppend(std::span<const std::byte> body, std::string_view contentType = {}) & {
    if (!body.empty() && contentType.empty()) {
      contentType = http::ContentTypeApplicationOctetStream;
    }
    return bodyAppend(std::string_view{reinterpret_cast<const char*>(body.data()), body.size()}, contentType);
  }

  // Rvalue overload for span-based append.
  HttpResponse&& bodyAppend(std::span<const std::byte> body, std::string_view contentType = {}) && {
    return std::move(bodyAppend(body, contentType));
  }

  // Same as string_view-based append, but accepts a C-string (null-terminated).
  // If the pointer is nullptr, it is treated as an empty chunk to append.
  HttpResponse& bodyAppend(const char* body, std::string_view contentType = {}) & {
    return bodyAppend(body == nullptr ? std::string_view() : std::string_view(body), contentType);
  }

  // Rvalue overload for C-string append.
  HttpResponse&& bodyAppend(const char* body, std::string_view contentType = {}) && {
    return std::move(bodyAppend(body, contentType));
  }

  // Appends directly inside the body up to 'maxLen' bytes of data.
  // 'writer' provides as a single argument the start of the buffer where to append body data and
  // should return the actual number of bytes written (should be <= 'maxLen').
  // If body was previously captured (including files), this will throw std::logic_error.
  // It is undefined behavior to write more than 'maxLen' bytes of data into the writer (for one call).
  // This is the most efficient way to set the inline body as it avoids copies and limits allocations.
  // Growing of the internal buffer is exponential. You can call this method several times (it will append data to
  // existing inline body).
  // To erase the body, call 'body' with an empty buffer.
  // ContentType is optional - if non-empty, it replaces current body content type.
  // Otherwise, initializes content type to 'application/octet-stream' if content type is not already set.
  template <class Writer>
  HttpResponse& bodyInlineAppend(std::size_t maxLen, Writer&& writer, std::string_view contentType = {}) & {
    if (!hasNoExternalPayload() && !_payloadVariant.isSizeOnly()) [[unlikely]] {
      throw std::logic_error("bodyInlineAppend can only be used with inline body responses");
    }
    if (_trailerLen != 0) [[unlikely]] {
      throw std::logic_error("Cannot set body after the first trailer");
    }

    contentType = TrimOws(contentType);

    using W = std::remove_reference_t<Writer>;
    // Accept writers callable as either: std::size_t(char*) or std::size_t(std::byte*)
    // and select a sensible default Content-Type based on the pointer type.
    std::string_view defaultContentType;
    if constexpr (std::is_invocable_r_v<std::size_t, W, std::byte*>) {
      defaultContentType = http::ContentTypeApplicationOctetStream;
    } else if constexpr (std::is_invocable_r_v<std::size_t, W, char*>) {
      defaultContentType = http::ContentTypeTextPlain;
    } else {
      static_assert(false, "Writer must be callable with either (char*) or (std::byte*) and return std::size_t");
    }

    const auto contentTypeValueSize = contentType.empty() ? defaultContentType.size() : contentType.size();
    const auto contentTypeHeaderSize = HeaderSize(http::ContentType.size(), contentTypeValueSize);
    const std::size_t oldBodyLen = _payloadVariant.isSizeOnly() ? _payloadVariant.size() : internalBodyAndTrailersLen();
    const auto maxBodyLen = oldBodyLen + maxLen;
    const auto contentLengthHeaderSize =
        HeaderSize(http::ContentLength.size(), static_cast<std::size_t>(nchars(maxBodyLen)));

    _data.ensureAvailableCapacityExponential(maxLen + contentTypeHeaderSize + contentLengthHeaderSize);

    bodyAppendUpdateHeaders(contentType, defaultContentType, maxBodyLen);

    std::size_t written;
    if constexpr (std::is_invocable_r_v<std::size_t, W, std::byte*>) {
      written = static_cast<std::size_t>(
          std::invoke(std::forward<Writer>(writer), reinterpret_cast<std::byte*>(_data.data() + _data.size())));
    } else if constexpr (std::is_invocable_r_v<std::size_t, W, char*>) {
      written = static_cast<std::size_t>(std::invoke(std::forward<Writer>(writer), _data.data() + _data.size()));
    }

    if (written == 0) {
      // No data written, remove the content-type header we just added if there is no body
      if (oldBodyLen == 0) {
        // erase both content-length and content-type headers
        _data.setSize(_data.size() - contentLengthHeaderSize - contentTypeHeaderSize - http::CRLF.size());
        _data.unchecked_append(http::CRLF);
        adjustBodyStart(-static_cast<int64_t>(contentLengthHeaderSize) - static_cast<int64_t>(contentTypeHeaderSize));
      } else {
        // we need to restore the previous content-length value
        const auto newBodyLenCharVec = IntegralToCharVector(maxBodyLen - (maxLen - written));
        replaceHeaderValueNoRealloc(getContentLengthValuePtr(maxBodyLen), std::string_view(newBodyLenCharVec));
      }
    } else {
      if (isHead()) {
        setHeadSize(written + oldBodyLen);
      } else {
        _data.addSize(written);
      }
      const auto newBodyLenCharVec = IntegralToCharVector(maxBodyLen - (maxLen - written));
      replaceHeaderValueNoRealloc(getContentLengthValuePtr(maxBodyLen), std::string_view(newBodyLenCharVec));
    }

    return *this;
  }

  // Rvalue overload that accepts a `std::byte*` writer.
  template <class Writer>
  HttpResponse&& bodyInlineAppend(std::size_t maxLen, Writer&& writer, std::string_view contentType = {}) && {
    return std::move(bodyInlineAppend(maxLen, std::forward<Writer>(writer), contentType));
  }

  // Sets (overwrites) the inline body directly from a writer callback up to 'maxLen' bytes.
  // 'writer' provides as a single argument the start of the buffer where to write body data and
  // should return the actual number of bytes written (should be <= 'maxLen').
  // Unlike bodyInlineAppend, this method replaces any existing body from the start.
  // If body was previously captured (e.g., via body(std::string)), this will erase it.
  // If trailers exist, this will throw std::logic_error.
  // It is undefined behavior to write more than 'maxLen' bytes of data into the writer (for one call).
  // This is the most efficient way to set the inline body as it avoids copies and uses exact capacity reservation
  // (no exponential growth).
  // To append to an existing body instead, use bodyInlineAppend.
  // If ContentType is non-empty, it replaces current body content type.
  // Otherwise, initializes content type based on writer pointer type:
  //   - std::byte* writer → 'application/octet-stream'
  //   - char* writer → 'text/plain'
  template <class Writer>
  HttpResponse& bodyInlineSet(std::size_t maxLen, Writer&& writer, std::string_view contentType = {}) & {
    using W = std::remove_reference_t<Writer>;
    if (_trailerLen != 0) [[unlikely]] {
      throw std::logic_error("Cannot set body after trailers have been added");
    }
    contentType = TrimOws(contentType);
    // Determine default content type based on writer signature
    std::string_view defaultContentType;
    if constexpr (std::is_invocable_r_v<std::size_t, W, std::byte*>) {
      defaultContentType = http::ContentTypeApplicationOctetStream;
    } else if constexpr (std::is_invocable_r_v<std::size_t, W, char*>) {
      defaultContentType = http::ContentTypeTextPlain;
    } else {
      static_assert(false, "Writer must be callable with either (char*) or (std::byte*) and return std::size_t");
    }
    if (contentType.empty()) {
      contentType = defaultContentType;
    }
    const auto contentTypeHeaderSize = HeaderSize(http::ContentType.size(), contentType.size());
    const auto contentLengthHeaderSize =
        HeaderSize(http::ContentLength.size(), static_cast<std::size_t>(nchars(maxLen)));

    // Reserve exact capacity (no exponential growth)
    _data.reserve(_data.size() + contentTypeHeaderSize + contentLengthHeaderSize + maxLen);

    bodyAppendUpdateHeaders(contentType, defaultContentType, maxLen);

    // Call writer at body start position
    std::size_t written;
    if constexpr (std::is_invocable_r_v<std::size_t, W, std::byte*>) {
      written = static_cast<std::size_t>(
          std::invoke(std::forward<Writer>(writer), reinterpret_cast<std::byte*>(_data.data() + _data.size())));
    } else if constexpr (std::is_invocable_r_v<std::size_t, W, char*>) {
      written = static_cast<std::size_t>(std::invoke(std::forward<Writer>(writer), _data.data() + _data.size()));
    }

    // If nothing was written, remove the content-type header
    if (written == 0) {
      // erase both content-length and content-type headers
      _data.setSize(_data.size() - contentLengthHeaderSize - contentTypeHeaderSize - http::CRLF.size() -
                    internalBodyAndTrailersLen());
      _data.unchecked_append(http::CRLF);
      setBodyStartPos(_data.size());
    } else {
      // Set final size
      if (isHead()) {
        setHeadSize(written);
      } else {
        _data.setSize(bodyStartPos() + written);
      }

      const auto newBodyLenCharVec = IntegralToCharVector(written);
      replaceHeaderValueNoRealloc(getContentLengthValuePtr(maxLen), std::string_view(newBodyLenCharVec));
    }

    // Clear any payload variant
    if (!isHead() || written == 0) {
      _payloadVariant = {};
    }
    return *this;
  }

  // Rvalue overload for bodyInlineSet.
  template <class Writer>
  HttpResponse&& bodyInlineSet(std::size_t maxLen, Writer&& writer, std::string_view contentType = {}) && {
    return std::move(bodyInlineSet(maxLen, std::forward<Writer>(writer), contentType));
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
    return std::move(file(std::move(fileObj), 0, 0, contentType));
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
    return std::move(file(std::move(fileObj), offset, length, contentType));
  }

  // Adds a trailer header to be sent after the response body (RFC 7230 §4.1.2).
  //
  // IMPORTANT ORDERING CONSTRAINT:
  //   Trailers MUST be added AFTER the body has been set (via body() or its overloads).
  //   If called before body is set, throws std::logic_error.
  //
  // Trailer semantics (per RFC 7230 §4.1.2):
  //   - Trailers are sent after the message body in chunked transfer encoding.
  //   - Certain headers MUST NOT appear as trailers (e.g., Transfer-Encoding, Content-Length,
  //     Host, Cache-Control, Authorization, Cookie, Set-Cookie). Use of forbidden trailer
  //     headers is undefined behavior (no validation is performed here for performance;
  //     validation may be added in debug builds).
  //   - Typical use: computed metadata available only after body generation (checksums,
  //     signatures, etc.).
  //   - Adding trailers for HTTP/1.1 has an additional transformation cost of the response.
  //     We need to switch to chunked transfer encoding and this will move internal parts
  //     of the buffer. If you use trailers frequently, consider using HTTP/2 which has a
  //     more efficient encoding for trailers, or HttpResponseWriter which manages this better.
  HttpResponse& trailerAddLine(std::string_view name, std::string_view value) &;

  // Adds a trailer header to be sent after the response body (RFC 7230 §4.1.2).
  HttpResponse&& trailerAddLine(std::string_view name, std::string_view value) && {
    return std::move(trailerAddLine(name, value));
  }

  // Convenient overload adding a trailer whose value is numeric.
  HttpResponse& trailerAddLine(std::string_view key, std::integral auto value) & {
    return trailerAddLine(key, std::string_view(IntegralToCharVector(value)));
  }

  // Convenient overload adding a trailer whose value is numeric.
  HttpResponse&& trailerAddLine(std::string_view key, std::integral auto value) && {
    return std::move(trailerAddLine(key, std::string_view(IntegralToCharVector(value))));
  }

  // Pre-allocate internal buffer capacity to avoid multiple allocations when building the response with headers and
  // inlined body.
  void reserve(std::size_t capacity) { _data.reserve(capacity); }

 private:
  friend class SingleHttpServer;
  friend class HttpRequest;
  friend class HttpResponseTest;
  friend class HttpResponseWriter;  // streaming writer needs access to finalize
  friend class internal::HttpCodec;
#ifdef AERONET_ENABLE_HTTP2
  friend class http2::Http2ProtocolHandler;
#endif

  enum class Empty : std::uint8_t { Yes };

  // Private constructor to avoid allocating memory for the data buffer when not needed immediately.
  // Use with care! All setters take the assumption that the internal buffer is available.
  explicit constexpr HttpResponse([[maybe_unused]] Empty empty) noexcept {}

  [[nodiscard]] constexpr bool isHead() const noexcept { return _knownOptions.isHeadMethod(); }

  constexpr void setHeadSize(std::size_t size) {
    _payloadVariant = HttpPayload(std::string_view(static_cast<const char*>(nullptr), size));
  }

  constexpr void setCapturedPayload(auto payload) {
    if (payload.empty()) {
      _payloadVariant = {};
    } else if (isHead()) {
      setHeadSize(payload.size());
    } else {
      _payloadVariant = HttpPayload(std::move(payload));
    }
  }

  constexpr void setCapturedPayload(auto payload, std::size_t size) {
    if (size == 0) {
      _payloadVariant = {};
    } else if (isHead()) {
      setHeadSize(size);
    } else {
      _payloadVariant = HttpPayload(std::move(payload), size);
    }
  }

  [[nodiscard]] std::string_view internalTrailers() const noexcept { return {_data.end() - _trailerLen, _data.end()}; }

  [[nodiscard]] std::string_view externalTrailers() const noexcept {
    const char* last = _payloadVariant.view().end();
    return {last - _trailerLen, last};
  }

  // Check if this HttpResponse has an inline body stored in its internal buffer.
  // Can be empty.
  [[nodiscard]] bool hasNoExternalPayload() const noexcept { return _payloadVariant.empty(); }

  [[nodiscard]] constexpr std::size_t internalBodyAndTrailersLen() const noexcept {
    return _data.size() - bodyStartPos();
  }

  void setBodyInternal(std::string_view newBody);

  enum class OnlyIfNew : std::uint8_t { No, Yes };

  // Return true if a new header was added or replaced.
  bool setHeader(std::string_view key, std::string_view value, OnlyIfNew onlyIfNew = OnlyIfNew::No);

  void setBodyHeaders(std::string_view contentTypeValue, std::size_t newBodySize, bool setContentTypeIfPresent = true);

  // Convert all header names to lower-case (for HTTP/2).
  void makeAllHeaderNamesLowerCase();

  [[nodiscard]] std::string_view headersFlatViewWithDate() const noexcept;

  // Simple bitmap class to pass finalization options with strong typing and better readability (passing several bools
  // is easy to get it wrong).
  class Options {
   public:
    static constexpr uint8_t Close = 1U << 0;
    static constexpr uint8_t AddTrailerHeader = 1U << 1;
    static constexpr uint8_t IsHeadMethod = 1U << 2;
    static constexpr uint8_t Prepared = 1U << 3;

    [[nodiscard]] constexpr bool isClose() const noexcept { return (_optionsBitmap & Close) != 0; }
    [[nodiscard]] constexpr bool isAddTrailerHeader() const noexcept {
      return (_optionsBitmap & AddTrailerHeader) != 0;
    }
    [[nodiscard]] constexpr bool isHeadMethod() const noexcept { return (_optionsBitmap & IsHeadMethod) != 0; }

    // Tells whether the response has been pre-configured already.
    // If it's the case, then global headers have already been applied, addTrailerHeader and headMethod options
    // are known. Close is only best effort - it may still be changed later (from not close to close).
    [[nodiscard]] constexpr bool isPrepared() const noexcept { return (_optionsBitmap & Prepared) != 0; }

    constexpr void close(bool val) noexcept {
      if (val) {
        _optionsBitmap |= Close;
      } else {
        _optionsBitmap &= static_cast<uint8_t>(~Close);
      }
    }

    constexpr void addTrailerHeader(bool val) noexcept {
      if (val) {
        _optionsBitmap |= AddTrailerHeader;
      } else {
        _optionsBitmap &= static_cast<uint8_t>(~AddTrailerHeader);
      }
    }

    constexpr void headMethod(bool val) noexcept {
      if (val) {
        _optionsBitmap |= IsHeadMethod;
      } else {
        _optionsBitmap &= static_cast<uint8_t>(~IsHeadMethod);
      }
    }

    constexpr void setPrepared() noexcept { _optionsBitmap |= Prepared; }

   private:
    uint8_t _optionsBitmap{};
  };

  // IMPORTANT: This method finalizes the response by appending reserved headers,
  // and returns the internal buffers stolen from this HttpResponse instance.
  // So this instance must not be used anymore after this call.
  HttpResponseData finalizeForHttp1(SysTimePoint tp, http::Version version, Options opts,
                                    const ConcatenatedHeaders* pGlobalHeaders, std::size_t minCapturedBodySize);

  constexpr FilePayload* filePayloadPtr() noexcept { return _payloadVariant.getIfFilePayload(); }

  [[nodiscard]] constexpr const FilePayload* filePayloadPtr() const noexcept {
    return _payloadVariant.getIfFilePayload();
  }

  void bodyAppendUpdateHeaders(std::string_view givenContentType, std::string_view defaultContentType,
                               std::size_t totalBodyLen);

  // header pos is stored in lower 16 bits, body pos in upper 48 bits
  static constexpr std::uint32_t kHeaderPosNbBits = 16U;

  static constexpr std::uint32_t kBodyPosNbBits = 64U - kHeaderPosNbBits;

  static constexpr std::uint64_t kHeadersStartMask = (std::uint64_t{1} << kHeaderPosNbBits) - 1;
  static constexpr std::uint64_t kBodyStartMask = (std::uint64_t{1} << kBodyPosNbBits) - 1;

  [[nodiscard]] constexpr std::uint64_t headersStartPos() const noexcept { return _posBitmap & kHeadersStartMask; }
  [[nodiscard]] constexpr std::uint64_t bodyStartPos() const noexcept {
    return (_posBitmap >> kHeaderPosNbBits) & kBodyStartMask;
  }

  constexpr void setHeadersStartPos(std::uint16_t pos) noexcept {
    _posBitmap = (_posBitmap & (kBodyStartMask << kHeaderPosNbBits)) | static_cast<std::uint64_t>(pos);
  }

  constexpr void setBodyStartPos(std::uint64_t pos) {
    assert(pos <= kBodyStartMask);
    _posBitmap = (_posBitmap & kHeadersStartMask) | (pos << kHeaderPosNbBits);
  }

  constexpr void adjustHeadersStart(int32_t diff) {
    setHeadersStartPos(static_cast<std::uint16_t>(static_cast<int64_t>(headersStartPos()) + diff));
  }

  constexpr void adjustBodyStart(int64_t diff) {
    setBodyStartPos(static_cast<std::uint64_t>(static_cast<int64_t>(bodyStartPos()) + diff));
  }

  char* getContentLengthHeaderLinePtr(std::size_t bodyLen) {
    const auto contentLengthHeaderLineSize =
        HeaderSize(http::ContentLength.size(), static_cast<std::size_t>(nchars(bodyLen)));
    return _data.data() + bodyStartPos() - http::DoubleCRLF.size() - contentLengthHeaderLineSize;
  }

  char* getContentLengthValuePtr(std::size_t bodyLen) {
    return getContentLengthHeaderLinePtr(bodyLen) + http::CRLF.size() + http::ContentLength.size() +
           http::HeaderSep.size();
  }

  // Returns a pointer to the beginning of the Content-Type header line (starting on CRLF before the header name).
  char* getContentTypeHeaderLinePtr(std::size_t bodyLen) {
    char* ptr = getContentLengthHeaderLinePtr(bodyLen) - HeaderSize(http::ContentType.size(), 0U);
    for (; *ptr != '\r'; --ptr) {
    }
    return ptr;
  }

  char* getContentTypeValuePtr(std::size_t bodyLen) {
    return getContentTypeHeaderLinePtr(bodyLen) + http::CRLF.size() + http::ContentType.size() + http::HeaderSep.size();
  }

  void replaceHeaderValueNoRealloc(char* first, std::string_view newValue);

  RawChars _data;
  // headersStartPos: the status line length, excluding CRLF.
  // bodyStartPos: position where the body starts (immediately after CRLFCRLF).
  // Bitmap layout: [48 bits bodyStartPos][16 bits headersStartPos]
  std::uint64_t _posBitmap{0};
  // Variant that can hold an external captured payload (HttpPayload).
  HttpPayload _payloadVariant;
  std::uint32_t _trailerLen{0};  // trailer length
  // When HEAD is known (prepared options), body/trailer storage can be suppressed while preserving lengths.
  Options _knownOptions;
};

}  // namespace aeronet
