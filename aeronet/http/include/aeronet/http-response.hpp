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
#include "aeronet/direct-compression-mode.hpp"
#include "aeronet/encoding.hpp"
#include "aeronet/file.hpp"
#include "aeronet/header-write.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-header-is-valid.hpp"
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
#include "aeronet/time-constants.hpp"
#include "aeronet/timedef.hpp"

namespace aeronet {

class EncoderContext;

namespace internal {
class HttpCodec;
struct ResponseCompressionState;
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
//   - Use HttpRequest::makeResponse() to construct a response from a request, which will pre-populate headers and
//     provide additional context to allow optimizations (HEAD, connection close, compression, etc)
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
 private:
  enum class Check : std::uint8_t { Yes, No };

  enum class BodySetContext : std::uint8_t { Inline, Captured };

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
           HeaderSize(http::ContentLength.size(), nchars(bodyLen));
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
  // The content type must be valid. Defaults to "text/plain"
  explicit HttpResponse(std::string_view body, std::string_view contentType = http::ContentTypeTextPlain)
      : HttpResponse(http::StatusCodeOK, body, contentType) {}

  // Same as above, but with a byte span for the body.
  explicit HttpResponse(std::span<const std::byte> body,
                        std::string_view contentType = http::ContentTypeApplicationOctetStream)
      : HttpResponse(std::string_view(reinterpret_cast<const char*>(body.data()), body.size()), contentType) {}

  // Constructs an HttpResponse with the given additional capacity, status code, concatenated headers,
  // body and content type. The body is copied into the internal buffer.
  // The concatenatedHeaders should follow a strict format. Each header key value pair MUST be formatted as:
  //   <HeaderName><http::HeaderSep><HeaderValue><http::CRLF>
  // Examples of concatenatedHeaders, for http::HeaderSep = ": " and http::CRLF = "\r\n":
  //   ""
  //   "HeaderName: Value\r\n"
  //   "HeaderName1: Value1\r\nHeaderName2: Value2\r\n"
  // Empty concatenatedHeaders are allowed.
  // Throws std::invalid_argument if the concatenatedHeaders format is invalid.
  HttpResponse(std::size_t additionalCapacity, http::StatusCode code, std::string_view concatenatedHeaders,
               std::string_view body = {}, std::string_view contentType = http::ContentTypeTextPlain)
      : HttpResponse(additionalCapacity, code, concatenatedHeaders, body, contentType, Check::Yes) {}

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
  [[nodiscard]] bool hasHeader(std::string_view key) const noexcept;

  // Retrieves the value of the first occurrence of the given header key (case-insensitive search per RFC 7230).
  // If the header is not found, returns std::nullopt.
  // Notes:
  //  - For HttpResponse that started direct automatic streaming compression, 'content-length' will not reflect the
  //    actual body length before the finalization.
  //  - The Date header cannot be retrieved nor changed, it it managed by aeronet.
  [[nodiscard]] std::optional<std::string_view> headerValue(std::string_view key) const noexcept;

  // Same as headerValue(), but returns an empty string_view instead of std::nullopt if the header is not found.
  // To distinguish between missing and present-but-empty header values, use headerValue().
  [[nodiscard]] std::string_view headerValueOrEmpty(std::string_view key) const noexcept;

  // Get a contiguous view of the current headers stored in this HttpResponse, except for the Date header which is
  // managed by aeronet. Each header line is formatted as: name + ": " + value + CRLF. If no headers are present, it
  // returns an empty view.
  [[nodiscard]] std::string_view headersFlatView() const noexcept {
    return {_data.data() + headersStartPos() + http::Date.size() + http::HeaderSep.size() + RFC7231DateStrLen +
                http::DoubleCRLF.size(),
            _data.data() + bodyStartPos() - http::CRLF.size()};
  }

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
    return hasBodyCaptured() ? (_payloadVariant.size() - trailersSize()) : bodyInlinedLength();
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
  [[nodiscard]] std::size_t bodyInlinedLength() const noexcept {
    return _data.size() - bodyStartPos() - trailersSize();
  }

  // Synonym for bodyInlinedLength().
  [[nodiscard]] std::size_t bodyInlinedSize() const noexcept { return bodyInlinedLength(); }

  // Returns the current direct compression mode for this HttpResponse.
  [[nodiscard]] DirectCompressionMode directCompressionMode() const noexcept { return _opts._directCompressionMode; }

  // Checks if the given trailer key is present (case-insensitive search per RFC 7230).
  [[nodiscard]] bool hasTrailer(std::string_view key) const noexcept;

  // Retrieves the value of the first occurrence of the given trailer key (case-insensitive search per RFC 7230).
  // If the trailer is not found, returns std::nullopt.
  [[nodiscard]] std::optional<std::string_view> trailerValue(std::string_view key) const noexcept;

  // Get the total size of all trailers, counting exactly one CRLF per trailer line.
  [[nodiscard]] std::size_t trailersSize() const noexcept { return _opts._trailerLen; }

  // Synonym for trailersSize().
  [[nodiscard]] std::size_t trailersLength() const noexcept { return trailersSize(); }

  // Retrieves the value of the first occurrence of the given trailer key (case-insensitive search per RFC 7230).
  // If the trailer is not found, returns an empty string_view.
  // To distinguish between missing and present-but-empty trailer values, use trailerValue().
  [[nodiscard]] std::string_view trailerValueOrEmpty(std::string_view key) const noexcept;

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

  // RValue overload of location(src).
  HttpResponse&& location(std::string_view src) && { return std::move(header(http::Location, src)); }

  // Inserts or replaces the Content-Encoding header.
  // Manually setting Content-Encoding header will disable automatic compression handling.
  // If you want to compress using codecs supported by aeronet (such as gzip, deflate, br and zstd),
  // it's recommended to not set Content-Encoding header manually and let the library handle compression.
  // If the data to be inserted references internal instance memory, the behavior is undefined.
  // It is forbidden to set content-encoding if the body is not empty, doing so will throw std::logic_error.
  HttpResponse& contentEncoding(std::string_view enc) & { return header(http::ContentEncoding, enc); }

  // RValue overload of contentEncoding(enc).
  HttpResponse&& contentEncoding(std::string_view enc) && { return std::move(header(http::ContentEncoding, enc)); }

  // Checks if this HttpResponse has a Content-Encoding header.
  [[nodiscard]] bool hasContentEncoding() const noexcept { return _opts.hasContentEncoding(); }

  // Append a header line (duplicates allowed, fastest path).
  // No scan over existing headers. Prefer this when duplicates are OK or when constructing headers once.
  // Header name and value must be valid per HTTP specifications.
  // Do not insert any reserved header (for which IsReservedResponseHeader is true), doing so is undefined behavior.
  // Attempting to set 'Content-Type' and 'Content-Length' headers with this method will throw std::invalid_argument.
  //  - Content-Type should be set along with the body methods
  //  - Content-Length is managed by the library and should not be set manually.
  // Similarly, 'Content-Encoding' header cannot be changed while a body is already set. Doing so will throw
  // std::logic_error.
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

  // Append 'value' to an existing header value, separated with 'sep', or call headerAddLine(key, value) if header
  // is missing. Example, from an empty HttpResponse, calling successively
  //   headerAppendValue("accept", "text/html", ", ")
  //   headerAppendValue("Accept", "application/json", ", ")
  // will produce:
  //   "accept: text/html"
  //   "accept: text/html, application/json"
  HttpResponse& headerAppendValue(std::string_view key, std::string_view value, std::string_view sep = ", ") &;

  // Rvalue overload of headerAppendValue.
  HttpResponse&& headerAppendValue(std::string_view key, std::string_view value, std::string_view sep = ", ") && {
    return std::move(headerAppendValue(key, value, sep));
  }

  // Convenient overload appending a numeric value.
  HttpResponse& headerAppendValue(std::string_view key, std::integral auto value, std::string_view sep = ", ") & {
    return headerAppendValue(key, std::string_view(IntegralToCharVector(value)), sep);
  }

  // Convenient overload appending a numeric value.
  HttpResponse&& headerAppendValue(std::string_view key, std::integral auto value, std::string_view sep = ", ") && {
    return std::move(headerAppendValue(key, std::string_view(IntegralToCharVector(value)), sep));
  }

  // Add or replace first header 'key' with 'value'.
  // Performs a linear scan (slower than headerAddLine()) using case-insensitive comparison of header names per
  // RFC 7230 (HTTP field names are case-insensitive). The original casing of the first occurrence is preserved in
  // HTTP1.x, but in HTTP/2 header names will be lowercased during serialization.
  // The header name and value must be valid per HTTP specifications.
  // As for 'headerAddLine()', do not insert any reserved header.
  HttpResponse& header(std::string_view key, std::string_view value) & {
    setHeader(key, value, OnlyIfNew::No);
    return *this;
  }

  // RValue overload of header(key, value).
  HttpResponse&& header(std::string_view key, std::string_view value) && { return std::move(header(key, value)); }

  // Convenient overload setting a header to a numeric value.
  HttpResponse& header(std::string_view key, std::integral auto value) & {
    setHeader(key, std::string_view(IntegralToCharVector(value)), OnlyIfNew::No);
    return *this;
  }

  // Convenient overload setting a header to a numeric value.
  HttpResponse&& header(std::string_view key, std::integral auto value) && {
    return std::move(header(key, std::string_view(IntegralToCharVector(value))));
  }

  // Remove the first occurrence of the header with the given key, search starting from backwards (case-insensitive
  // search per RFC 7230). If the header is not found, the HttpResponse is not modified.
  // Content-type and Content-Length headers cannot be removed, as they are managed by aeronet based on the body
  // content.
  HttpResponse& headerRemoveLine(std::string_view key) &;

  // RValue overload of headerRemoveLine.
  HttpResponse&& headerRemoveLine(std::string_view key) && { return std::move(headerRemoveLine(key)); }

  // Remove the first 'value' from the header with the given key, search starting from backwards (case-insensitive
  // search per RFC 7230). If the value is the only one for the header, the whole header line is removed. If there are
  // multiple values for the header, only the first specified value is removed (starting from the beginning) and the
  // other values are kept, according to the split made by given 'sep'. If the header or value is not found, the
  // HttpResponse is not modified. Separator must not be empty, and should be the same as the one used in
  // headerAppendValue() for the same header. The behavior is undefined if the header values can contain the separator
  // string.
  HttpResponse& headerRemoveValue(std::string_view key, std::string_view value, std::string_view sep = ", ") &;

  // RValue overload of headerRemoveValue.
  HttpResponse&& headerRemoveValue(std::string_view key, std::string_view value, std::string_view sep = ", ") && {
    return std::move(headerRemoveValue(key, value, sep));
  }

  // -------------/
  // BODY SETTERS /
  // -------------/

  // Override the direct compression mode for this HttpResponse.
  // Note that this will not have any effect if the HttpResponse has not been constructed with
  // HttpRequest::makeResponse().
  // HEAD responses never activate direct compression to avoid extra CPU work; headers reflect
  // the uncompressed body size and no Content-Encoding is added.
  HttpResponse& directCompressionMode(DirectCompressionMode mode) & {
    _opts._directCompressionMode = mode;
    return *this;
  }

  // Rvalue overload of directCompressionMode(mode).
  HttpResponse&& directCompressionMode(DirectCompressionMode mode) && { return std::move(directCompressionMode(mode)); }

  // Assigns the given body to this HttpResponse.
  // Empty body is allowed - this will remove any existing body.
  // The whole buffer is copied internally in the HttpResponse. If the body is large, prefer the capture by value of
  // body() overloads to avoid a copy (and possibly an allocation).
  // If the HttpResponse is eligible for direct compression (see directCompressionMode()), the body will be
  // compressed in-place in the internal buffer.
  // If content-type is omitted, it will be set to "text/plain" by default.
  // If the Body referencing internal memory of this HttpResponse is undefined behavior.
  HttpResponse& body(std::string_view body, std::string_view contentType = http::ContentTypeTextPlain) & {
    setBodyHeaders(contentType, body.size(), BodySetContext::Inline);
    setBodyInternal(body);
    if (isHead()) {
      setHeadSize(body.size());
    } else {
      _payloadVariant = {};
    }
    return *this;
  }

  // Rvalue overload of body(std::string_view, ...).
  HttpResponse&& body(std::string_view body, std::string_view contentType = http::ContentTypeTextPlain) && {
    return std::move(this->body(body, contentType));
  }

  // Same as body(std::string_view body, ...) but with a byte span for the body, and 'application/octet-stream' as the
  // default content type.
  HttpResponse& body(std::span<const std::byte> body,
                     std::string_view contentType = http::ContentTypeApplicationOctetStream) & {
    return this->body(std::string_view{reinterpret_cast<const char*>(body.data()), body.size()}, contentType);
  }

  // Rvalue overload of body(std::span<const std::byte>, ...).
  HttpResponse&& body(std::span<const std::byte> body,
                      std::string_view contentType = http::ContentTypeApplicationOctetStream) && {
    return std::move(
        this->body(std::string_view{reinterpret_cast<const char*>(body.data()), body.size()}, contentType));
  }

  // Same as body(std::string_view body, ...) but with a C-string for the body.
  HttpResponse& body(const char* body, std::string_view contentType = http::ContentTypeTextPlain) & {
    return this->body(body == nullptr ? std::string_view() : std::string_view(body), contentType);
  }

  // Rvalue overload of body(const char*, ...).
  HttpResponse&& body(const char* body, std::string_view contentType = http::ContentTypeTextPlain) && {
    return std::move(this->body(body, contentType));
  }

  // Capture the body to avoid a copy.
  // Requires an rvalue reference to avoid accidental copies of std::string.
  // The body is simply moved into this HttpResponse without any copy until the transport layer (if no compression
  // happens). Empty body is allowed - this will remove any existing body. The content type must be valid. Defaults to
  // "text/plain".
  // It is possible to call 'bodyAppend()' on the moved std::string - this will call std::string::append() on the
  // captured std::string.
  HttpResponse& body(std::string&& body, std::string_view contentType = http::ContentTypeTextPlain) & {
    setBodyHeaders(contentType, body.size(), BodySetContext::Captured);
    setBodyInternal(std::string_view{});
    setCapturedPayload(std::move(body));
    return *this;
  }

  // Rvalue overload of body(std::string&&, ...).
  HttpResponse&& body(std::string&& body, std::string_view contentType = http::ContentTypeTextPlain) && {
    return std::move(this->body(std::move(body), contentType));
  }

  // Same as above, but with a vector of char for the body, and 'application/octet-stream' as the default content type.
  HttpResponse& body(std::vector<char>&& body,
                     std::string_view contentType = http::ContentTypeApplicationOctetStream) & {
    setBodyHeaders(contentType, body.size(), BodySetContext::Captured);
    setBodyInternal(std::string_view{});
    setCapturedPayload(std::move(body));
    return *this;
  }

  // Rvalue overload of body(std::vector<char>&&, ...).
  HttpResponse&& body(std::vector<std::byte>&& body,
                      std::string_view contentType = http::ContentTypeApplicationOctetStream) && {
    return std::move(this->body(std::move(body), contentType));
  }

  // Same as above, but with a vector of byte for the body, and 'application/octet-stream' as the default content type.
  HttpResponse& body(std::vector<std::byte>&& body,
                     std::string_view contentType = http::ContentTypeApplicationOctetStream) & {
    setBodyHeaders(contentType, body.size(), BodySetContext::Captured);
    setBodyInternal(std::string_view{});
    setCapturedPayload(std::move(body));
    return *this;
  }

  // Rvalue overload of body(std::vector<char>&&, ...).
  HttpResponse&& body(std::vector<char>&& body,
                      std::string_view contentType = http::ContentTypeApplicationOctetStream) && {
    return std::move(this->body(std::move(body), contentType));
  }

  // Same as above, but with a unique_ptr to a char array with its size, and 'application/octet-stream' as the default
  // content type.
  // The behavior is undefined if the char buffer actual size is different from the provided size.
  // The body is moved into this HttpResponse without any copy until the transport layer (if no compression happens).
  // Empty body is allowed (size=0) - this will remove any existing body. The content type must be valid. Defaults to
  // 'application/octet-stream'.
  // If 'bodyAppend()' is called after this, aeronet will automatically allocate a buffer and copy the captured body
  // into it before appending the new data.
  HttpResponse& body(std::unique_ptr<char[]> body, std::size_t size,
                     std::string_view contentType = http::ContentTypeApplicationOctetStream) & {
    setBodyHeaders(contentType, size, BodySetContext::Captured);
    setBodyInternal(std::string_view{});
    setCapturedPayload(std::move(body), size);
    return *this;
  }

  // Rvalue overload of body(std::unique_ptr<char[]>&&, ...).
  HttpResponse&& body(std::unique_ptr<char[]> body, std::size_t size,
                      std::string_view contentType = http::ContentTypeApplicationOctetStream) && {
    return std::move(this->body(std::move(body), size, contentType));
  }

  // Same as body(std::unique_ptr<char[]>, ...), but with a unique_ptr to a byte array for the body.
  HttpResponse& body(std::unique_ptr<std::byte[]> body, std::size_t size,
                     std::string_view contentType = http::ContentTypeApplicationOctetStream) & {
    setBodyHeaders(contentType, size, BodySetContext::Captured);
    setBodyInternal(std::string_view{});
    setCapturedPayload(std::move(body), size);
    return *this;
  }

  // Rvalue overload of body(std::unique_ptr<std::byte[]>&&, ...).
  HttpResponse&& body(std::unique_ptr<std::byte[]> body, std::size_t size,
                      std::string_view contentType = http::ContentTypeApplicationOctetStream) && {
    return std::move(this->body(std::move(body), size, contentType));
  }

  // Sets the body of this HttpResponse to point to a static buffer.
  // This can be useful for large static content like HTML pages, images, etc. that are known at compile time and have a
  // lifetime that exceeds the HttpResponse, until its data is conveyed to the transport layer.
  // Internally, this will capture the provided std::string_view.
  // Note that if bodyAppend() is called after bodyStatic(), aeronet will automatically allocate a buffer.
  HttpResponse& bodyStatic(std::string_view staticBody, std::string_view contentType = http::ContentTypeTextPlain) & {
    setBodyHeaders(contentType, staticBody.size(), BodySetContext::Captured);
    setBodyInternal(std::string_view{});
    setCapturedPayload(staticBody);
    return *this;
  }

  // Rvalue overload for string_view-based static body.
  HttpResponse&& bodyStatic(std::string_view staticBody, std::string_view contentType = http::ContentTypeTextPlain) && {
    return std::move(this->bodyStatic(staticBody, contentType));
  }

  // Same as string_view-based bodyStatic, but accepts a span of bytes, and defaults content type to
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
  // - If `body` is empty this call is a no-op - it appends nothing and does NOT clear any existing body.
  //   To clear the body explicitly use `body("")` or one of the `body(...)` overloads with an empty value.
  // - `contentType` is optional. If non-empty it replaces the current Content-Type header.
  //   If empty and no Content-Type header exists yet, the header is set to
  //   `text/plain` only when the appended data is non-empty.
  // - Safe to call multiple times; data is appended to any existing inline body.
  // Trailers should not be added before calling this method.
  // It is compatible with direct compression mode if activated for this HttpResponse, and will internally use streaming
  // compression.
  HttpResponse& bodyAppend(std::string_view body, std::string_view contentType = {}) &;

  // Rvalue overload of string_view-based bodyAppend.
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

  // Same as string_view-based append, but accepts a C-string (it should be null-terminated).
  // If the pointer is nullptr, it is treated as an empty chunk to append.
  HttpResponse& bodyAppend(const char* body, std::string_view contentType = {}) & {
    return bodyAppend(body == nullptr ? std::string_view() : std::string_view(body), contentType);
  }

  // Rvalue overload for C-string append.
  HttpResponse&& bodyAppend(const char* body, std::string_view contentType = {}) && {
    return std::move(bodyAppend(body, contentType));
  }

  // Sets (overwrites) the inline body directly from a writer callback up to 'maxLen' bytes.
  // 'writer' provides as a single argument the start of the buffer where to write body data and
  // should return the actual number of bytes written (should be <= 'maxLen').
  // Unlike bodyInlineAppend, this method replaces any existing body from the start.
  // If body was previously captured (e.g., via body(std::string)), this will erase it.
  // If trailers exist, this will throw std::logic_error.
  // It is undefined behavior to write more than 'maxLen' bytes of data into the writer (for one call).
  // This is an efficient way to set the inline body as it avoids copies and uses exact capacity reservation
  // (no exponential growth).
  // However, it is not compatible with direct automatic compression because the zero-copy would not be guaranteed.
  // To append to an existing body instead, use bodyInlineAppend. If ContentType is non-empty,
  // it replaces current body content type. Otherwise, initializes content type based on writer pointer type:
  //   - std::byte* writer → 'application/octet-stream'
  //   - char* writer → 'text/plain'
  template <class Writer>
  HttpResponse& bodyInlineSet(std::size_t maxLen, Writer&& writer, std::string_view contentType = {}) & {
    using W = std::remove_reference_t<Writer>;
    bodyPrecheckContentType(contentType);

    if (contentType.empty()) {
      // Determine default content type based on writer signature
      if constexpr (std::is_invocable_r_v<std::size_t, W, std::byte*>) {
        contentType = http::ContentTypeApplicationOctetStream;
      } else if constexpr (std::is_invocable_r_v<std::size_t, W, char*>) {
        contentType = http::ContentTypeTextPlain;
      } else {
        static_assert(false, "Writer must be callable with either (char*) or (std::byte*) and return std::size_t");
      }
    }

    if (bodyLength() != 0 || _opts.isAutomaticDirectCompression()) {
      removeBodyAndItsHeaders();
      // Clear any payload variant
      _payloadVariant = {};
    }

    const auto contentTypeHeaderSize = HeaderSize(http::ContentType.size(), contentType.size());
    const auto contentLengthHeaderSize = HeaderSize(http::ContentLength.size(), nchars(maxLen));

    // Reserve exact capacity (no exponential growth)
    _data.reserve(_data.size() + contentTypeHeaderSize + contentLengthHeaderSize + maxLen);

    char* insertPtr = addContentTypeAndContentLengthHeaders(contentType, maxLen);

    // Call writer at body start position
    std::size_t written;
    if constexpr (std::is_invocable_r_v<std::size_t, W, std::byte*>) {
      written =
          static_cast<std::size_t>(std::invoke(std::forward<Writer>(writer), reinterpret_cast<std::byte*>(insertPtr)));
    } else {
      written = static_cast<std::size_t>(std::invoke(std::forward<Writer>(writer), insertPtr));
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
        _data.setSize(static_cast<std::size_t>(insertPtr + written - _data.data()));
      }

      const auto newBodyLenCharVec = IntegralToCharVector(written);
      replaceHeaderValueNoRealloc(getContentLengthValuePtr(), std::string_view(newBodyLenCharVec));
    }

    return *this;
  }

  // Rvalue overload for bodyInlineSet.
  template <class Writer>
  HttpResponse&& bodyInlineSet(std::size_t maxLen, Writer&& writer, std::string_view contentType = {}) && {
    return std::move(bodyInlineSet(maxLen, std::forward<Writer>(writer), contentType));
  }

  // Appends directly inside the body up to 'maxLen' bytes of data.
  // 'writer' provides as a single argument the start of the buffer where to append body data and
  // should return the actual number of bytes written (should be <= 'maxLen').
  // If body was previously captured (including files), this will throw std::logic_error.
  // It is undefined behavior to write more than 'maxLen' bytes of data into the writer (for one call).
  // This is an efficient way to set the inline body as it avoids copies and limits allocations.
  // Growing of the internal buffer is exponential.
  // You can call this method several times (it will append data to existing inline body).
  // However, it is not compatible with direct automatic compression because the zero-copy would not be guaranteed.
  // To erase the body, call 'body' with an empty buffer.
  // ContentType is optional - if non-empty, it replaces current body content type.
  // Otherwise, initializes content type to 'application/octet-stream' if content type is not already set.
  template <class Writer>
  HttpResponse& bodyInlineAppend(std::size_t maxLen, Writer&& writer, std::string_view contentType = {}) & {
    if (!hasNoExternalPayload() && !_payloadVariant.isSizeOnly()) [[unlikely]] {
      throw std::logic_error("bodyInlineAppend can only be used with inline body responses");
    }
    bodyPrecheckContentType(contentType);

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
    const auto nCharsMaxBodyLen = nchars(maxBodyLen);
    const auto contentLengthHeaderSize = HeaderSize(http::ContentLength.size(), nCharsMaxBodyLen);

    std::size_t neededCapacity = contentTypeHeaderSize + contentLengthHeaderSize + maxLen;
    if (_opts.isAutomaticDirectCompression()) {
      // Not ideal - we started a streaming compression and client now calls bodyInlineAppend which is not compatible
      // with direct compression. So we will write the body uncompressed and then apply compression to the whole body at
      // the end, which is not zero-copy but still correct.
      neededCapacity += maxLen;
    }

    _data.ensureAvailableCapacityExponential(neededCapacity);

    bodyAppendUpdateHeaders(contentType, defaultContentType, maxBodyLen);

    char* first =
        _opts.isAutomaticDirectCompression() ? _data.data() + _data.size() + maxLen : _data.data() + _data.size();

    std::size_t written;
    if constexpr (std::is_invocable_r_v<std::size_t, W, std::byte*>) {
      written =
          static_cast<std::size_t>(std::invoke(std::forward<Writer>(writer), reinterpret_cast<std::byte*>(first)));
    } else {
      written = static_cast<std::size_t>(std::invoke(std::forward<Writer>(writer), first));
    }

    if (written == 0) {
      // No data written, remove the content-type header we just added if there is no body
      if (oldBodyLen == 0 && !_opts.isAutomaticDirectCompression()) {
        // erase both content-length and content-type headers
        _data.setSize(_data.size() - contentLengthHeaderSize - contentTypeHeaderSize - http::CRLF.size());
        _data.unchecked_append(http::CRLF);
        adjustBodyStart(-static_cast<int64_t>(contentLengthHeaderSize) - static_cast<int64_t>(contentTypeHeaderSize));
      } else {
        // we need to restore the previous content-length value
        const auto newBodyLenCharVec = IntegralToCharVector(maxBodyLen - (maxLen - written));
        replaceHeaderValueNoRealloc(getContentLengthValuePtr(), std::string_view(newBodyLenCharVec));
      }
    } else {
#if defined(AERONET_ENABLE_BROTLI) || defined(AERONET_ENABLE_ZLIB) || defined(AERONET_ENABLE_ZSTD)
      if (_opts.isAutomaticDirectCompression()) {
        // during streaming compression, if the output buffer is too small,
        // encoders do NOT fail — they keep compressed data in their internal state and wait for more output space.
        written = appendEncodedInlineOrThrow(false, std::string_view(first, first + written), maxLen);
      }
#endif
      if (isHead()) {
        setHeadSize(written + oldBodyLen);
      } else {
        _data.addSize(written);
      }
      const auto newBodyLenCharVec = IntegralToCharVector(maxBodyLen - (maxLen - written));
      replaceHeaderValueNoRealloc(getContentLengthValuePtr(), std::string_view(newBodyLenCharVec));
    }

    return *this;
  }

  // Rvalue overload that accepts a `std::byte*` writer.
  template <class Writer>
  HttpResponse&& bodyInlineAppend(std::size_t maxLen, Writer&& writer, std::string_view contentType = {}) && {
    return std::move(bodyInlineAppend(maxLen, std::forward<Writer>(writer), contentType));
  }

  // Stream the contents of an already-open file as the response body.
  // This methods takes ownership of the 'file' object into the response and sends the entire file.
  // Notes:
  //   - file should be opened (`file` must be true)
  //   - Trailers are NOT permitted when using file
  //   - Errors: filesystem read/write errors are surfaced during transmission; callers should expect the connection
  //     to be closed on fatal I/O failures.
  //   - Content Type header: if non-empty, sets given content type value. Otherwise, attempt to guess it from the
  //     file object. If the MIME type is unknown, sets 'application/octet-stream' as Content type.
  HttpResponse& file(File fileObj, std::string_view contentType = {}) & {
    return file(std::move(fileObj), 0, 0, contentType);
  }

  // RValue overload of file(File, ...).
  HttpResponse&& file(File fileObj, std::string_view contentType = {}) && {
    return std::move(file(std::move(fileObj), 0, 0, contentType));
  }

  // Same as above, but with specified offset and length for the file content to be sent. If length is 0, it means
  // "until the end of the file". So to clear the file (or body) payload, use body("") instead.
  HttpResponse& file(File fileObj, std::size_t offset, std::size_t length, std::string_view contentType = {}) &;

  // Rvalue overload of file(fileObj, offset, length).
  HttpResponse&& file(File fileObj, std::size_t offset, std::size_t length, std::string_view contentType = {}) && {
    return std::move(file(std::move(fileObj), offset, length, contentType));
  }

  // Adds a trailer header to be sent after the response body (RFC 7230 §4.1.2).
  // The header name and value must be valid per HTTP specifications.
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
  //     more efficient encoding for trailers, or HttpResponseWriter which manages this natively
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
  // The capacity should be enough to hold the entire response (status line, headers, body if inlined, trailers and the
  // CRLF chars) to avoid reallocations.
  void reserve(std::size_t capacity) { _data.reserve(capacity); }

 private:
  friend class SingleHttpServer;
  friend class HttpRequest;
  friend class HttpResponseTest;
  friend class HttpResponseWriter;  // streaming writer needs access to finalize
  friend class internal::HttpCodec;
  friend class StaticFileHandler;
#ifdef AERONET_ENABLE_HTTP2
  friend class http2::Http2ProtocolHandler;
#endif

  // Private constructor to avoid allocating memory for the data buffer when not needed immediately.
  // Use with care! All setters currently take the assumption that the internal buffer is allocated.
  explicit constexpr HttpResponse([[maybe_unused]] Check check) noexcept {}

  // Private constructor bypassing checks for internal use only.
  HttpResponse(std::size_t additionalCapacity, http::StatusCode code, std::string_view concatenatedHeaders,
               std::string_view body, std::string_view contentType, Check check);

  [[nodiscard]] constexpr bool isHead() const noexcept { return _opts.isHeadMethod(); }

  constexpr void setHeadSize(std::size_t size) {
    _payloadVariant = HttpPayload(std::string_view(static_cast<const char*>(nullptr), size));
  }

  void headerAddLineUnchecked(std::string_view key, std::string_view value);

  // warning: this method should only be called if you are sure that the header already exists.
  void overrideHeaderUnchecked(const char* oldValueFirst, const char* oldValueLast, std::string_view newValue);

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

  [[nodiscard]] std::string_view internalTrailers() const noexcept {
    return {_data.end() - trailersSize(), _data.end()};
  }

  [[nodiscard]] std::string_view externalTrailers() const noexcept {
    const char* last = _payloadVariant.view().end();
    return {last - trailersSize(), last};
  }

  // Check if this HttpResponse has an inline body stored in its internal buffer.
  // Can be empty.
  [[nodiscard]] bool hasNoExternalPayload() const noexcept { return _payloadVariant.empty(); }

  [[nodiscard]] constexpr std::size_t internalBodyAndTrailersLen() const noexcept {
    return _data.size() - bodyStartPos();
  }

  enum class OnlyIfNew : std::uint8_t { No, Yes };

  // Return true if a new header was added or replaced.
  bool setHeader(std::string_view key, std::string_view value, OnlyIfNew onlyIfNew = OnlyIfNew::No);

  void setBodyHeaders(std::string_view contentTypeValue, std::size_t newBodySize, BodySetContext context);

  void setBodyInternal(std::string_view newBody);

#ifdef AERONET_ENABLE_HTTP2
  void finalizeForHttp2();
#endif

  [[nodiscard]] std::string_view headersFlatViewWithDate() const noexcept {
    return {_data.data() + headersStartPos() + http::CRLF.size(), _data.data() + bodyStartPos() - http::CRLF.size()};
  }

  // Same as headersFlatView but without Content-Type and Content-Length headers.
  [[nodiscard]] std::string_view headersFlatViewWithoutCTCL() const noexcept {
    return {_data.data() + headersStartPos() + http::Date.size() + http::HeaderSep.size() + RFC7231DateStrLen +
                http::DoubleCRLF.size(),
            getContentTypeHeaderLinePtr() + http::CRLF.size()};
  }

  // Simple bitmap class to pass finalization options with strong typing and better readability (passing several bools
  // is easy to get it wrong).
  class Options {
   public:
    using BmpType = uint8_t;

    static constexpr BmpType Close = 1U << 0;
    static constexpr BmpType AddTrailerHeader = 1U << 1;
    static constexpr BmpType IsHeadMethod = 1U << 2;
    static constexpr BmpType Prepared = 1U << 3;
    static constexpr BmpType AddVaryAcceptEncoding = 1U << 4;
    static constexpr BmpType HasContentEncoding = 1U << 5;
    static constexpr BmpType AutomaticDirectCompression = 1U << 6;

    Options() noexcept = default;

#if defined(AERONET_ENABLE_BROTLI) || defined(AERONET_ENABLE_ZLIB) || defined(AERONET_ENABLE_ZSTD)
    Options(internal::ResponseCompressionState& compressionState, Encoding expectedEncoding);
#endif

    [[nodiscard]] constexpr bool isClose() const noexcept { return (_optionsBitmap & Close) != 0; }
    [[nodiscard]] constexpr bool isAddTrailerHeader() const noexcept {
      return (_optionsBitmap & AddTrailerHeader) != 0;
    }
    [[nodiscard]] constexpr bool isHeadMethod() const noexcept { return (_optionsBitmap & IsHeadMethod) != 0; }

    [[nodiscard]] constexpr bool isAddVaryAcceptEncoding() const noexcept {
      return (_optionsBitmap & AddVaryAcceptEncoding) != 0;
    }

    [[nodiscard]] constexpr bool hasContentEncoding() const noexcept {
      return (_optionsBitmap & HasContentEncoding) != 0;
    }

    [[nodiscard]] constexpr bool isAutomaticDirectCompression() const noexcept {
      return (_optionsBitmap & AutomaticDirectCompression) != 0;
    }

    // Tells whether the response has been pre-configured already.
    // If it's the case, then global headers have already been applied, addTrailerHeader and headMethod options
    // are known. Close is only best effort - it may still be changed later (from not close to close).
    [[nodiscard]] constexpr bool isPrepared() const noexcept { return (_optionsBitmap & Prepared) != 0; }

    constexpr void close(bool val) noexcept {
      if (val) {
        _optionsBitmap |= Close;
      } else {
        _optionsBitmap &= static_cast<BmpType>(~Close);
      }
    }

    constexpr void addTrailerHeader(bool val) noexcept {
      if (val) {
        _optionsBitmap |= AddTrailerHeader;
      } else {
        _optionsBitmap &= static_cast<BmpType>(~AddTrailerHeader);
      }
    }

    constexpr void headMethod(bool val) noexcept {
      if (val) {
        _optionsBitmap |= IsHeadMethod;
      } else {
        _optionsBitmap &= static_cast<BmpType>(~IsHeadMethod);
      }
    }

    constexpr void addVaryAcceptEncoding(bool val) noexcept {
      if (val) {
        _optionsBitmap |= AddVaryAcceptEncoding;
      } else {
        _optionsBitmap &= static_cast<BmpType>(~AddVaryAcceptEncoding);
      }
    }

    constexpr void setHasContentEncoding(bool val) noexcept {
      if (val) {
        _optionsBitmap |= HasContentEncoding;
      } else {
        _optionsBitmap &= static_cast<BmpType>(~HasContentEncoding);
      }
    }

    constexpr void setAutomaticDirectCompression(bool val) noexcept {
      if (val) {
        _optionsBitmap |= AutomaticDirectCompression;
      } else {
        _optionsBitmap &= static_cast<BmpType>(~AutomaticDirectCompression);
      }
    }

    constexpr void setPrepared() noexcept { _optionsBitmap |= Prepared; }

    [[nodiscard]] constexpr bool directCompressionPossible() const noexcept {
#if defined(AERONET_ENABLE_BROTLI) || defined(AERONET_ENABLE_ZLIB) || defined(AERONET_ENABLE_ZSTD)
      return _pickedEncoding != Encoding::none && _directCompressionMode != DirectCompressionMode::Off;
#else
      return false;
#endif
    }

#if defined(AERONET_ENABLE_BROTLI) || defined(AERONET_ENABLE_ZLIB) || defined(AERONET_ENABLE_ZSTD)
    [[nodiscard]] bool directCompressionPossible(std::size_t bodySize,
                                                 std::string_view contentType = {}) const noexcept;
#endif

   private:
    friend class HttpResponse;
    friend class internal::HttpCodec;

#if defined(AERONET_ENABLE_BROTLI) || defined(AERONET_ENABLE_ZLIB) || defined(AERONET_ENABLE_ZSTD)
    internal::ResponseCompressionState* _pCompressionState{nullptr};
#endif

    std::uint32_t _trailerLen{0};  // trailer length - no logical reason to be there, it's just to benefit from packing
    BmpType _optionsBitmap{};
    Encoding _pickedEncoding{Encoding::none};
    DirectCompressionMode _directCompressionMode{DirectCompressionMode::Off};
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

  char* getContentLengthValueEndPtr() { return _data.data() + bodyStartPos() - http::DoubleCRLF.size(); }
  [[nodiscard]] const char* getContentLengthValueEndPtr() const {
    return _data.data() + bodyStartPos() - http::DoubleCRLF.size();
  }

  char* getContentLengthValuePtr() {
    char* ptr = getContentLengthValueEndPtr() - http::HeaderSep.size() - 1U;
    for (; *ptr != ':'; --ptr) {
    }
    return ptr + http::HeaderSep.size();
  }

  // Returns a pointer to the beginning of the Content-Length header line (starting on CRLF before the header name).
  char* getContentLengthHeaderLinePtr() {
    char* ptr =
        getContentLengthValueEndPtr() - http::HeaderSep.size() - http::ContentLength.size() - http::CRLF.size() - 1U;
    for (; *ptr != '\r'; --ptr) {
    }
    return ptr;
  }
  [[nodiscard]] const char* getContentLengthHeaderLinePtr() const {
    const char* ptr =
        getContentLengthValueEndPtr() - http::HeaderSep.size() - http::ContentLength.size() - http::CRLF.size() - 1U;
    for (; *ptr != '\r'; --ptr) {
    }
    return ptr;
  }

  // Returns a pointer to the beginning of the Content-Type header line (starting on CRLF before the header name).
  char* getContentTypeHeaderLinePtr() {
    char* ptr = getContentLengthHeaderLinePtr() - HeaderSize(http::ContentType.size(), http::ContentTypeMinLen);
    for (; *ptr != '\r'; --ptr) {
    }
    return ptr;
  }

  [[nodiscard]] const char* getContentTypeHeaderLinePtr() const {
    const char* ptr = getContentLengthHeaderLinePtr() - HeaderSize(http::ContentType.size(), http::ContentTypeMinLen);
    for (; *ptr != '\r'; --ptr) {
    }
    return ptr;
  }

  char* getContentTypeValuePtr() {
    char* ptr = getContentLengthHeaderLinePtr() - http::HeaderSep.size() - http::ContentTypeMinLen;
    for (; *ptr != ':'; --ptr) {
    }
    return ptr + http::HeaderSep.size();
  }

  void bodyPrecheckContentType(std::string_view& contentType) const {
    if (trailersSize() != 0) [[unlikely]] {
      throw std::logic_error("Cannot set body after trailers have been added");
    }
    contentType = TrimOws(contentType);
    if (!contentType.empty() && !http::IsValidHeaderValue(contentType)) [[unlikely]] {
      throw std::invalid_argument("Invalid Content-Type header value");
    }
  }

  void replaceHeaderValueNoRealloc(char* first, std::string_view newValue);

#if defined(AERONET_ENABLE_BROTLI) || defined(AERONET_ENABLE_ZLIB) || defined(AERONET_ENABLE_ZSTD)
  // Returns the number of written bytes
  std::size_t appendEncodedInlineOrThrow(bool init, std::string_view data, std::size_t capacity);

  void finalizeInlineBody(int64_t additionalCapacity = 0);
#endif

  void removeBodyAndItsHeaders();

  // Add Content-Type and Content-Length headers for a new body, erasing any existing body and its headers if needed.
  // Returns a pointer to the position where the body should be written (immediately after the CRLFCRLF sequence).
  char* addContentTypeAndContentLengthHeaders(std::string_view contentType, std::size_t bodySize) {
    char* insertPtr =
        WriteHeaderCRLF(http::ContentType, contentType, _data.data() + bodyStartPos() - http::CRLF.size());
    insertPtr = WriteHeader(http::ContentLength, bodySize, insertPtr);
    insertPtr = Append(http::DoubleCRLF, insertPtr);

    const auto bodyStart = static_cast<std::uint64_t>(insertPtr - _data.data());
    setBodyStartPos(bodyStart);
    _data.setSize(bodyStart);
    return insertPtr;
  }

  RawChars _data;
  // headersStartPos: the status line length, excluding CRLF.
  // bodyStartPos: position where the body starts (immediately after CRLFCRLF).
  // Bitmap layout: [48 bits bodyStartPos][16 bits headersStartPos]
  std::uint64_t _posBitmap;
  // Variant that can hold an external captured payload (HttpPayload).
  HttpPayload _payloadVariant;
  // When HEAD is known (prepared options), body/trailer storage can be suppressed while preserving lengths.
  Options _opts;
};

}  // namespace aeronet
