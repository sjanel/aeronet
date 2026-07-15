#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>

#include "aeronet/concatenated-headers.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-message-data.hpp"
#include "aeronet/http-message.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/simple-charconv.hpp"
#include "aeronet/time-constants.hpp"

namespace aeronet {

#ifdef AERONET_ENABLE_HTTP2
namespace http2 {
class Http2ProtocolHandler;
class Http2WriterTransport;
}  // namespace http2
#endif

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
//   - Use HttpRequestView::makeResponse() to construct a response from a request, which will pre-populate headers and
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
//     this external buffer, otherwise they are appended to the internal HttpMessage buffer
//   - Streaming responses: `HttpResponseWriter` implements a separate streaming-safe
//     `trailerAddLine()` API which buffers trailer lines during streaming and emits them
//     after the final zero-length chunk (see `HttpResponseWriter` docs).
// -----------------------------------------------------------------------------
class HttpResponse final : public HttpMessage {
 public:
  // "HTTP/x.y". Should be changed if version major / minor exceed 1 digit
  static constexpr std::size_t kHttp1VersionLen = http::HTTP10Sv.size();
  static constexpr std::size_t kStatusCodeBeg = kHttp1VersionLen + 1;  // index of first status code digit
  static constexpr std::size_t kReasonBeg = kStatusCodeBeg + 3 + 1;    // index of first reason phrase character

  // Minimum initial capacity for HttpMessage internal buffer to avoid too-small allocations.
  // The minimal valid HTTP response that will be returned by aeronet is (note the mandatory SP after the
  // status code even when the reason-phrase is empty, per RFC 9112 §4):
  // "HTTP/1.1 200 \r\nDate: Tue, 07 Jan 2025 12:34:56 GMT\r\n\r\n" (54 bytes).
  static constexpr std::size_t kHttpResponseMinInitialCapacity = 54U + 17U;

  // Constructs an HttpMessage with a StatusCode OK (200) and a default initial capacity.
  HttpResponse() : HttpResponse(http::StatusCodeOK) {}

  // Constructs an HttpMessage with the given status code and a default initial capacity.
  explicit HttpResponse(http::StatusCode code) : HttpResponse(kHttpResponseMinInitialCapacity, code) {}

  // Constructs an HttpMessage with the given status code and body, that will be copied into the internal buffer.
  HttpResponse(http::StatusCode code, std::string_view body, std::string_view contentType = http::ContentTypeTextPlain);

  // Constructs an HttpMessage with an additional initial capacity for the internal buffer.
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
      : HttpResponse(additionalCapacity, code, concatenatedHeaders, body, contentType, HttpMessage::Check::Yes) {}

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
  [[nodiscard]] std::size_t statusLineSize() const noexcept { return dateHeaderStartPos() + http::CRLF.size(); }

  // Synonym for statusLineSize().
  [[nodiscard]] std::size_t statusLineLength() const noexcept { return statusLineSize(); }

  // Get the current reason stored in this HttpResponse, or an empty string_view if no reason is set.
  [[nodiscard]] std::string_view reason() const noexcept { return {_data.data() + kReasonBeg, reasonLength()}; }

  // Check if a reason phrase is present.
  [[nodiscard]] bool hasReason() const noexcept { return _data[kReasonBeg] != '\n'; }

  // Get the length of the current reason stored in this HttpResponse.
  [[nodiscard]] std::size_t reasonLength() const noexcept {
    return (dateHeaderStartPos() - kReasonBeg) * static_cast<std::size_t>(hasReason());
  }

  // Synonym for reasonLength().
  [[nodiscard]] std::size_t reasonSize() const noexcept { return reasonLength(); }

  // Get the current file stored in this HttpResponse, or nullptr if no file is set.
  [[nodiscard]] const File* file() const noexcept { return HttpMessage::file(); }

  // Returns the current direct compression mode for this HttpMessage.
  [[nodiscard]] DirectCompressionMode directCompressionMode() const noexcept {
    return HttpMessage::directCompressionMode();
  }

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
  HttpResponse& headerAddLine(std::string_view key, std::string_view value) & {
    HttpMessage::headerAddLine(key, value);
    return *this;
  }

  // Rvalue overload of headerAddLine.
  HttpResponse&& headerAddLine(std::string_view key, std::string_view value) && {
    return std::move(headerAddLine(key, value));
  }

  // Convenient overload adding a header whose value is numeric.
  HttpResponse& headerAddLine(std::string_view key, std::integral auto value) & {
    HttpMessage::headerAddLine(key, value);
    return *this;
  }

  // Convenient overload adding a header whose value is numeric.
  HttpResponse&& headerAddLine(std::string_view key, std::integral auto value) && {
    return std::move(headerAddLine(key, value));
  }

  // Append 'value' to an existing header value, separated with 'sep', or call headerAddLine(key, value) if header
  // is missing. Example, from an empty HttpMessage, calling successively
  //   headerAppendValue("accept", "text/html", ", ")
  //   headerAppendValue("Accept", "application/json", ", ")
  // will produce:
  //   "accept: text/html"
  //   "accept: text/html, application/json"
  HttpResponse& headerAppendValue(std::string_view key, std::string_view value, std::string_view sep = ", ") & {
    HttpMessage::headerAppendValue(key, value, sep);
    return *this;
  }

  // Rvalue overload of headerAppendValue.
  HttpResponse&& headerAppendValue(std::string_view key, std::string_view value, std::string_view sep = ", ") && {
    return std::move(headerAppendValue(key, value, sep));
  }

  // Convenient overload appending a numeric value.
  HttpResponse& headerAppendValue(std::string_view key, std::integral auto value, std::string_view sep = ", ") & {
    HttpMessage::headerAppendValue(key, value, sep);
    return *this;
  }

  // Convenient overload appending a numeric value.
  HttpResponse&& headerAppendValue(std::string_view key, std::integral auto value, std::string_view sep = ", ") && {
    return std::move(headerAppendValue(key, value, sep));
  }

  // Add or replace first header 'key' with 'value'.
  // Performs a linear scan (slower than headerAddLine()) using case-insensitive comparison of header names per
  // RFC 7230 (HTTP field names are case-insensitive). The original casing of the first occurrence is preserved in
  // HTTP1.x, but in HTTP/2 header names will be lowercased during serialization.
  // The header name and value must be valid per HTTP specifications.
  // As for 'headerAddLine()', do not insert any reserved header.
  HttpResponse& header(std::string_view key, std::string_view value) & {
    HttpMessage::header(key, value);
    return *this;
  }

  // RValue overload of header(key, value).
  HttpResponse&& header(std::string_view key, std::string_view value) && { return std::move(header(key, value)); }

  // Convenient overload setting a header to a numeric value.
  HttpResponse& header(std::string_view key, std::integral auto value) & {
    HttpMessage::header(key, value);
    return *this;
  }

  // Convenient overload setting a header to a numeric value.
  HttpResponse&& header(std::string_view key, std::integral auto value) && { return std::move(header(key, value)); }

  // Remove the first occurrence of the header with the given key, search starting from backwards (case-insensitive
  // search per RFC 7230). If the header is not found, the HttpMessage is not modified.
  // Content-type and Content-Length headers cannot be removed, as they are managed by aeronet based on the body
  // content.
  HttpResponse& headerRemoveLine(std::string_view key) & {
    HttpMessage::headerRemoveLine(key);
    return *this;
  }

  // RValue overload of headerRemoveLine.
  HttpResponse&& headerRemoveLine(std::string_view key) && { return std::move(headerRemoveLine(key)); }

  // Remove the first 'value' from the header with the given key, search starting from backwards (case-insensitive
  // search per RFC 7230). If the value is the only one for the header, the whole header line is removed. If there are
  // multiple values for the header, only the first specified value is removed (starting from the beginning) and the
  // other values are kept, according to the split made by given 'sep'. If the header or value is not found, the
  // HttpMessage is not modified. Separator must not be empty, and should be the same as the one used in
  // headerAppendValue() for the same header. The behavior is undefined if the header values can contain the separator
  // string.
  HttpResponse& headerRemoveValue(std::string_view key, std::string_view value, std::string_view sep = ", ") & {
    HttpMessage::headerRemoveValue(key, value, sep);
    return *this;
  }

  // RValue overload of headerRemoveValue.
  HttpResponse&& headerRemoveValue(std::string_view key, std::string_view value, std::string_view sep = ", ") && {
    return std::move(headerRemoveValue(key, value, sep));
  }

  // -------------/
  // BODY SETTERS /
  // -------------/

  // Override the direct compression mode for this HttpMessage.
  // Note that this will not have any effect if the HttpMessage has not been constructed with
  // HttpRequestView::makeResponse().
  // HEAD responses never activate direct compression to avoid extra CPU work; headers reflect
  // the uncompressed body size and no Content-Encoding is added.
  HttpResponse& directCompressionMode(DirectCompressionMode mode) & {
    _opts._directCompressionMode = mode;
    return *this;
  }

  // Rvalue overload of directCompressionMode(mode).
  HttpResponse&& directCompressionMode(DirectCompressionMode mode) && { return std::move(directCompressionMode(mode)); }

  // Assigns the given body to this HttpMessage.
  // Empty body is allowed - this will remove any existing body.
  // The whole buffer is copied internally in the HttpMessage. If the body is large, prefer the capture by value of
  // body() overloads to avoid a copy (and possibly an allocation).
  // If the HttpMessage is eligible for direct compression (see directCompressionMode()), the body will be
  // compressed in-place in the internal buffer.
  // If content-type is omitted, it will be set to "text/plain" by default.
  // If the Body referencing internal memory of this HttpMessage is undefined behavior.
  HttpResponse& body(std::string_view body, std::string_view contentType = http::ContentTypeTextPlain) & {
    HttpMessage::body(body, contentType);
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
  // The body is simply moved into this HttpMessage without any copy until the transport layer (if no compression
  // happens). Empty body is allowed - this will remove any existing body. The content type must be valid. Defaults to
  // "text/plain".
  // It is possible to call 'bodyAppend()' on the moved std::string - this will call std::string::append() on the
  // captured std::string.
  HttpResponse& body(std::string&& body, std::string_view contentType = http::ContentTypeTextPlain) & {
    HttpMessage::body(std::move(body), contentType);
    return *this;
  }

  // Rvalue overload of body(std::string&&, ...).
  HttpResponse&& body(std::string&& body, std::string_view contentType = http::ContentTypeTextPlain) && {
    return std::move(this->body(std::move(body), contentType));
  }

  // Same as above, but with a vector of char for the body, and 'application/octet-stream' as the default content type.
  HttpResponse& body(std::vector<char>&& body,
                     std::string_view contentType = http::ContentTypeApplicationOctetStream) & {
    HttpMessage::body(std::move(body), contentType);
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
    HttpMessage::body(std::move(body), contentType);
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
  // The body is moved into this HttpMessage without any copy until the transport layer (if no compression happens).
  // Empty body is allowed (size=0) - this will remove any existing body. The content type must be valid. Defaults to
  // 'application/octet-stream'.
  // If 'bodyAppend()' is called after this, aeronet will automatically allocate a buffer and copy the captured body
  // into it before appending the new data.
  HttpResponse& body(std::unique_ptr<char[]> body, std::size_t size,
                     std::string_view contentType = http::ContentTypeApplicationOctetStream) & {
    HttpMessage::body(std::move(body), size, contentType);
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
    HttpMessage::body(std::move(body), size, contentType);
    return *this;
  }

  // Rvalue overload of body(std::unique_ptr<std::byte[]>&&, ...).
  HttpResponse&& body(std::unique_ptr<std::byte[]> body, std::size_t size,
                      std::string_view contentType = http::ContentTypeApplicationOctetStream) && {
    return std::move(this->body(std::move(body), size, contentType));
  }

#ifdef AERONET_ENABLE_GLAZE
  /// Serialize 'obj' as JSON directly into the response body (Content-Type: application/json).
  /// Avoids intermediate copies: Glaze writes into a std::string which is then moved into the body.
  /// Throws std::runtime_error on serialization failure (e.g. from a faulty custom Glaze serializer).
  /// Definition lives in <aeronet/http-json.hpp> (include it to use this method) so that translation
  /// units that do not serialize JSON/YAML do not pay the (heavy) Glaze compilation cost.
  template <class T>
  HttpResponse& bodyJson(const T& obj) & {
    HttpMessage::bodyJson(obj);
    return *this;
  }

  /// Rvalue overload of bodyJson. See bodyJson(const T&) & and include <aeronet/http-json.hpp>.
  template <class T>
  HttpResponse&& bodyJson(const T& obj) && {
    return std::move(bodyJson(obj));
  }

  /// Serialize 'obj' as YAML directly into the response body (Content-Type: text/yaml).
  /// Avoids intermediate copies: Glaze writes into a std::string which is then moved into the body.
  /// Throws std::runtime_error on serialization failure (e.g. from a faulty custom Glaze serializer).
  /// Definition lives in <aeronet/http-json.hpp> (include it to use this method).
  template <class T>
  HttpResponse& bodyYaml(const T& obj) & {
    HttpMessage::bodyYaml(obj);
    return *this;
  }

  /// Rvalue overload of bodyYaml. See bodyYaml(const T&) & and include <aeronet/http-json.hpp>.
  template <class T>
  HttpResponse&& bodyYaml(const T& obj) && {
    return std::move(bodyYaml(obj));
  }
#endif

  // Sets the body of this HttpResponse to point to a static buffer.
  // This can be useful for large static content like HTML pages, images, etc. that are known at compile time and have a
  // lifetime that exceeds the HttpResponse, until its data is conveyed to the transport layer.
  // Internally, this will capture the provided std::string_view.
  // Note that if bodyAppend() is called after bodyStatic(), aeronet will automatically allocate a buffer.
  HttpResponse& bodyStatic(std::string_view staticBody, std::string_view contentType = http::ContentTypeTextPlain) & {
    HttpMessage::bodyStatic(staticBody, contentType);
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
  // It is compatible with direct compression mode if activated for this HttpMessage, and will internally use streaming
  // compression.
  HttpResponse& bodyAppend(std::string_view body, std::string_view contentType = {}) & {
    HttpMessage::bodyAppend(body, contentType);
    return *this;
  }

  // Rvalue overload of string_view-based bodyAppend.
  HttpResponse&& bodyAppend(std::string_view body, std::string_view contentType = {}) && {
    return std::move(this->bodyAppend(body, contentType));
  }

  // Same as string_view-based append, but accepts a span of bytes, and defaults content type to
  // 'application/octet-stream' if not specified and body is non-empty.
  HttpResponse& bodyAppend(std::span<const std::byte> body, std::string_view contentType = {}) & {
    HttpMessage::bodyAppend(body, contentType);
    return *this;
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
    HttpMessage::bodyInlineSet(maxLen, std::forward<Writer>(writer), contentType);
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
    HttpMessage::bodyInlineAppend(maxLen, std::forward<Writer>(writer), contentType);
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
    HttpMessage::file(std::move(fileObj), 0, 0, contentType);
    return *this;
  }

  // RValue overload of file(File, ...).
  HttpResponse&& file(File fileObj, std::string_view contentType = {}) && {
    return std::move(file(std::move(fileObj), 0, 0, contentType));
  }

  // Same as above, but with specified offset and length for the file content to be sent. If length is 0, it means
  // "until the end of the file". So to clear the file (or body) payload, use body("") instead.
  HttpResponse& file(File fileObj, std::size_t offset, std::size_t length, std::string_view contentType = {}) & {
    HttpMessage::file(std::move(fileObj), offset, length, contentType);
    return *this;
  }

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
  HttpResponse& trailerAddLine(std::string_view name, std::string_view value) & {
    HttpMessage::trailerAddLine(name, value);
    return *this;
  }

  // Adds a trailer header to be sent after the response body (RFC 7230 §4.1.2).
  HttpResponse&& trailerAddLine(std::string_view name, std::string_view value) && {
    return std::move(trailerAddLine(name, value));
  }

  // Convenient overload adding a trailer whose value is numeric.
  HttpResponse& trailerAddLine(std::string_view key, std::integral auto value) & {
    HttpMessage::trailerAddLine(key, value);
    return *this;
  }

  // Convenient overload adding a trailer whose value is numeric.
  HttpResponse&& trailerAddLine(std::string_view key, std::integral auto value) && {
    return std::move(trailerAddLine(key, value));
  }

 private:
  friend class HttpRequestView;
  friend class HttpResponseTest;
  friend class HttpResponseWriter;  // streaming writer needs access to finalize
  friend class SingleHttpServer;
  friend class StaticFileHandler;
  friend class internal::Http1WriterTransport;  // HTTP/1.1 transport for streaming
#ifdef AERONET_ENABLE_HTTP2
  friend class http2::Http2ProtocolHandler;
  friend class http2::Http2WriterTransport;
#endif
#ifdef AERONET_ENABLE_HTTP_CLIENT
  friend class HttpClient;
#endif

  // The RFC does not specify a maximum length for the reason phrase, but in practice it should be reasonable.
  // It's not really used by clients, as they mostly rely on the status code instead.
  // We store the header status line on 16 bits, so the reason must have a maximum length of 2^16 - 1 - kReasonBeg.
  static constexpr std::uint32_t kMaxReasonLength =
      (1U << kHeaderPosNbBits) - 1U - kReasonBeg - http::HeaderSize(http::Date.size(), RFC7231DateStrLen);

  // Private constructor to avoid allocating memory for the data buffer when not needed immediately.
  // Use with care! All setters currently take the assumption that the internal buffer is allocated.
  explicit constexpr HttpResponse([[maybe_unused]] Check check) noexcept : HttpMessage(check) {}

  // Private constructor bypassing checks for internal use only.
  HttpResponse(std::size_t additionalCapacity, http::StatusCode code, std::string_view concatenatedHeaders,
               std::string_view body, std::string_view contentType, Check check);

  [[nodiscard]] std::string_view headersFlatViewWithDate() const noexcept {
    return {_data.data() + dateHeaderStartPos() + http::CRLF.size(), _data.data() + bodyStartPos() - http::CRLF.size()};
  }

  [[nodiscard]] std::size_t dateHeaderStartPos() const noexcept {
    return headersStartPos() - http::HeaderSize(http::Date.size(), RFC7231DateStrLen);
  }

#ifdef AERONET_ENABLE_HTTP_CLIENT
  // Deep-copy this message into an independent, fully-owning HttpMessage. HttpMessage is otherwise move-only
  // (it may own a move-only captured payload), so this is the explicit way to duplicate one: it copies the
  // head buffer, the normalization state and the (in-memory) body -- a captured body is re-materialized into
  // an owning buffer, behaviourally identical since a payload is only ever consumed as a byte view. Intended
  // for callers that must retain a copy of a message they do not own (e.g. HttpClient's response cache).
  // File payloads and HEAD size-only payloads are not supported (asserted): a parsed client response, the
  // only current caller, always inlines its body, so it never carries either.
  [[nodiscard]] HttpResponse cloneFinalized() const {
    HttpResponse copy(HttpMessage::Check{});
    copy._data = _data;
    copy._posBitmap = _posBitmap;
    assert(_payloadVariant.empty());
    copy._opts = _opts;
    return copy;
  }
#endif

  // IMPORTANT: This method finalizes the response by appending reserved headers,
  // and returns the internal buffers stolen from this HttpMessage instance.
  // So this instance must not be used anymore after this call.
  HttpMessageData finalizeForHttp1(SysTimePoint tp, http::Version version, Options opts,
                                   const ConcatenatedHeaders* pGlobalHeaders, std::size_t minCapturedBodySize) {
    // Write the Http version (1.0 or 1.1)
    version.writeFull(_data.data());

    // Write date header
    WriteCRLFDateHeader(tp, _data.data() + dateHeaderStartPos());

    HttpMessage::finalizeForHttp1(version, opts, pGlobalHeaders, minCapturedBodySize);

    HttpMessageData prepared(std::move(_data), std::move(_payloadVariant));

    if (opts.isHeadMethod()) {
      auto* pFilePayload = prepared.getIfFilePayload();
      if (pFilePayload != nullptr) {
        pFilePayload->length = 0;
      }
    }

    return prepared;
  }
};

}  // namespace aeronet
