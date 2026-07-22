#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>

#include "aeronet/http-constants.hpp"
#include "aeronet/http-message-data.hpp"
#include "aeronet/http-message.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-version.hpp"

namespace aeronet {

struct DecompressionConfig;

namespace internal {
class ClientConnection;
struct HttpClientCodec;
struct UrlParseResult;
}  // namespace internal

// A contiguous single-buffer HTTP/1.x friendly request builder focused on minimal
// allocations and cache-friendly writes, optionally supporting large bodies captured
// in the request. It is also used as the basis for HTTP/2 request serialization,
// so that the API is common between HTTP/1.x and HTTP/2 requests.
//
// Memory layout:
//   [<Method> SP <Target> SP HTTP-Version CRLF][CRLF][CRLF]  (DoubleCRLF sentinel)
//   ^             ^               ^           ^   ^
//   |             |               |           |   +-- part of DoubleCRLF
//   |             |               |           +------ end of request line
//   |             |               +-- beginning of HTTP-Version
//   |             +-- beginning of Target
//   +-- start
// Internally, the HttpRequest contains at the beginning of the buffer the origin key (scheme://host:port) of the
// request URL, which is not part of the HTTP request itself, but is used for internal purposes.
class HttpRequest final : public HttpMessage {
 public:
  // --------/
  // GETTERS /
  // --------/

  // Get the current method stored in this HttpRequest.
  [[nodiscard]] http::Method method() const noexcept {
    switch (_data[_originKeyLen]) {
      case 'G':
        return http::Method::GET;
      case 'H':
        return http::Method::HEAD;
      case 'P':
        switch (_data[_originKeyLen + 1]) {
          case 'O':
            return http::Method::POST;
          case 'U':
            return http::Method::PUT;
          default:
            return http::Method::PATCH;
        }
      case 'D':
        return http::Method::DELETE;
      case 'C':
        return http::Method::CONNECT;
      case 'O':
        return http::Method::OPTIONS;
      default:
        return http::Method::TRACE;
    }
  }

  // Get the scheme of the request URL ("http" or "https").
  [[nodiscard]] std::string_view scheme() const noexcept {
    return {_data.data(), 4U + static_cast<uint8_t>(isTlsRequest())};
  }

  // Get the host of the request URL, without port.
  [[nodiscard]] std::string_view host() const noexcept {
    return {_data.data() + 7U + static_cast<uint8_t>(isTlsRequest()), _hostLen};
  }

  // Get the origin key of the request URL ("scheme://host:port").
  [[nodiscard]] std::string_view originKey() const noexcept { return {_data.data(), _originKeyLen}; }

  // Tells whether this request is a TLS request (https) or not (http). This is determined by the scheme of the URL used
  // to construct the request.
  [[nodiscard]] bool isTlsRequest() const noexcept { return _data[4U] == 's'; }

  // Get the port number of the request URL.
  [[nodiscard]] uint16_t port() const noexcept { return _port; }

  // Get the value of the Host header of this Http request.
  // It contains the port if is is non default (!= 443 for HTTPS, != 80 for HTTP).
  [[nodiscard]] std::string_view hostHeaderValue() const noexcept { return headerValueOrEmpty(http::Host); }

  // Get the size of the status line including CRLF (with method, target, HTTP version).
  [[nodiscard]] std::size_t statusLineSize() const noexcept {
    return headersStartPos() + http::CRLF.size() - _originKeyLen;
  }

  // Synonym for statusLineSize().
  [[nodiscard]] std::size_t statusLineLength() const noexcept { return statusLineSize(); }

  // Get the current target stored in this HttpRequest.
  [[nodiscard]] std::string_view target() const noexcept { return {targetBeg(), targetEnd()}; }

  // Get the current file stored in this HttpRequest, or nullptr if no file is set.
  [[nodiscard]] const File* file() const noexcept { return HttpMessage::file(); }

  // Returns the current direct compression mode for this HttpMessage.
  [[nodiscard]] DirectCompressionMode directCompressionMode() const noexcept {
    return HttpMessage::directCompressionMode();
  }

  // ---------------/
  // METHOD SETTERS /
  // ---------------/

  // Replaces the method.
  HttpRequest& method(http::Method method) &;

  // Rvalue overload of method(method).
  HttpRequest&& method(http::Method method) && { return std::move(this->method(method)); }

  // ---------------/
  // TARGET SETTERS /
  // ---------------/

  // Composition of a URL into its components (scheme, host, port, target) and the origin key (scheme://host:port):
  //
  //   "https://example.com:443/path?query"
  //    \____/  \_________/ \_/ \__________/
  //    scheme     host    port    target
  //    \_____________________/
  //           originKey

  // Replaces the request target. The target must be valid per HTTP specifications.
  // For a request to a non TLS proxy, the target must be an absolute-form URL (scheme://host:port/path?query).
  HttpRequest& target(std::string_view target) &;

  // Rvalue overload of target(target).
  HttpRequest&& target(std::string_view target) && { return std::move(this->target(target)); }

  // ---------------/
  // HEADER SETTERS /
  // ---------------/

  // Inserts or replaces the Content-Encoding header.
  // Manually setting Content-Encoding header will disable automatic compression handling.
  // If you want to compress using codecs supported by aeronet (such as gzip, deflate, br and zstd),
  // it's recommended to not set Content-Encoding header manually and let the library handle compression.
  // If the data to be inserted references internal instance memory, the behavior is undefined.
  // It is forbidden to set content-encoding if the body is not empty, doing so will throw std::logic_error.
  HttpRequest& contentEncoding(std::string_view enc) & { return header(http::ContentEncoding, enc); }

  // RValue overload of contentEncoding(enc).
  HttpRequest&& contentEncoding(std::string_view enc) && { return std::move(header(http::ContentEncoding, enc)); }

  // Append a header line (duplicates allowed, fastest path).
  // No scan over existing headers. Prefer this when duplicates are OK or when constructing headers once.
  // Header name and value must be valid per HTTP specifications.
  // Do not insert any reserved header (for which IsReservedOrForbiddenRequestHeader is true), doing so is undefined
  // behavior. Attempting to set 'Content-Type' and 'Content-Length' headers with this method will throw
  // std::invalid_argument.
  //  - Content-Type should be set along with the body methods
  //  - Content-Length is managed by the library and should not be set manually.
  // Similarly, 'Content-Encoding' header cannot be changed while a body is already set. Doing so will throw
  // std::logic_error.
  // If the data to be inserted references internal instance memory, the behavior is undefined.
  HttpRequest& headerAddLine(std::string_view key, std::string_view value) & {
    HttpMessage::headerAddLine(key, value);
    return *this;
  }

  // Rvalue overload of headerAddLine.
  HttpRequest&& headerAddLine(std::string_view key, std::string_view value) && {
    return std::move(headerAddLine(key, value));
  }

  // Convenient overload adding a header whose value is numeric.
  HttpRequest& headerAddLine(std::string_view key, std::integral auto value) & {
    HttpMessage::headerAddLine(key, value);
    return *this;
  }

  // Convenient overload adding a header whose value is numeric.
  HttpRequest&& headerAddLine(std::string_view key, std::integral auto value) && {
    return std::move(headerAddLine(key, value));
  }

  // Append 'value' to an existing header value, separated with 'sep', or call headerAddLine(key, value) if header
  // is missing. Example, from an empty HttpMessage, calling successively
  //   headerAppendValue("accept", "text/html", ", ")
  //   headerAppendValue("Accept", "application/json", ", ")
  // will produce:
  //   "accept: text/html"
  //   "accept: text/html, application/json"
  HttpRequest& headerAppendValue(std::string_view key, std::string_view value, std::string_view sep = ", ") & {
    HttpMessage::headerAppendValue(key, value, sep);
    return *this;
  }

  // Rvalue overload of headerAppendValue.
  HttpRequest&& headerAppendValue(std::string_view key, std::string_view value, std::string_view sep = ", ") && {
    return std::move(headerAppendValue(key, value, sep));
  }

  // Convenient overload appending a numeric value.
  HttpRequest& headerAppendValue(std::string_view key, std::integral auto value, std::string_view sep = ", ") & {
    HttpMessage::headerAppendValue(key, value, sep);
    return *this;
  }

  // Convenient overload appending a numeric value.
  HttpRequest&& headerAppendValue(std::string_view key, std::integral auto value, std::string_view sep = ", ") && {
    return std::move(headerAppendValue(key, value, sep));
  }

  // Add or replace first header 'key' with 'value'.
  // Performs a linear scan (slower than headerAddLine()) using case-insensitive comparison of header names per
  // RFC 7230 (HTTP field names are case-insensitive). The original casing of the first occurrence is preserved in
  // HTTP1.x, but in HTTP/2 header names will be lowercased during serialization.
  // The header name and value must be valid per HTTP specifications.
  // As for 'headerAddLine()', do not insert any reserved header.
  HttpRequest& header(std::string_view key, std::string_view value) & {
    HttpMessage::header(key, value);
    return *this;
  }

  // RValue overload of header(key, value).
  HttpRequest&& header(std::string_view key, std::string_view value) && { return std::move(header(key, value)); }

  // Convenient overload setting a header to a numeric value.
  HttpRequest& header(std::string_view key, std::integral auto value) & {
    HttpMessage::header(key, value);
    return *this;
  }

  // Convenient overload setting a header to a numeric value.
  HttpRequest&& header(std::string_view key, std::integral auto value) && { return std::move(header(key, value)); }

  // Remove the first occurrence of the header with the given key, search starting from backwards (case-insensitive
  // search per RFC 7230). If the header is not found, the HttpMessage is not modified.
  // Content-type and Content-Length headers cannot be removed, as they are managed by aeronet based on the body
  // content.
  HttpRequest& headerRemoveLine(std::string_view key) & {
    HttpMessage::headerRemoveLine(key);
    return *this;
  }

  // RValue overload of headerRemoveLine.
  HttpRequest&& headerRemoveLine(std::string_view key) && { return std::move(headerRemoveLine(key)); }

  // Remove the first 'value' from the header with the given key, search starting from backwards (case-insensitive
  // search per RFC 7230). If the value is the only one for the header, the whole header line is removed. If there are
  // multiple values for the header, only the first specified value is removed (starting from the beginning) and the
  // other values are kept, according to the split made by given 'sep'. If the header or value is not found, the
  // HttpMessage is not modified. Separator must not be empty, and should be the same as the one used in
  // headerAppendValue() for the same header. The behavior is undefined if the header values can contain the separator
  // string.
  HttpRequest& headerRemoveValue(std::string_view key, std::string_view value, std::string_view sep = ", ") & {
    HttpMessage::headerRemoveValue(key, value, sep);
    return *this;
  }

  // RValue overload of headerRemoveValue.
  HttpRequest&& headerRemoveValue(std::string_view key, std::string_view value, std::string_view sep = ", ") && {
    return std::move(headerRemoveValue(key, value, sep));
  }

  // -------------/
  // BODY SETTERS /
  // -------------/

  // Override the direct compression mode for this HttpRequest.
  // Note that this will not have any effect if the HttpRequest has not been constructed with
  // HttpClient::makeRequest().
  HttpRequest& directCompressionMode(DirectCompressionMode mode) & {
    _opts._directCompressionMode = mode;
    return *this;
  }

  // Rvalue overload of directCompressionMode(mode).
  HttpRequest&& directCompressionMode(DirectCompressionMode mode) && { return std::move(directCompressionMode(mode)); }

  // Assigns the given body to this HttpRequest.
  // Empty body is allowed - this will remove any existing body.
  // The whole buffer is copied internally in the HttpRequest. If the body is large, prefer the capture by value of
  // body() overloads to avoid a copy (and possibly an allocation).
  // If the HttpRequest is eligible for direct compression (see directCompressionMode()), the body will be
  // compressed in-place in the internal buffer.
  // If content-type is omitted, it will be set to "text/plain" by default.
  // If the Body referencing internal memory of this HttpRequest is undefined behavior.
  HttpRequest& body(std::string_view body, std::string_view contentType = http::ContentTypeTextPlain) & {
    HttpMessage::body(body, contentType);
    return *this;
  }

  // Rvalue overload of body(std::string_view, ...).
  HttpRequest&& body(std::string_view body, std::string_view contentType = http::ContentTypeTextPlain) && {
    return std::move(this->body(body, contentType));
  }

  // Same as body(std::string_view body, ...) but with a byte span for the body, and 'application/octet-stream' as the
  // default content type.
  HttpRequest& body(std::span<const std::byte> body,
                    std::string_view contentType = http::ContentTypeApplicationOctetStream) & {
    return this->body(std::string_view{reinterpret_cast<const char*>(body.data()), body.size()}, contentType);
  }

  // Rvalue overload of body(std::span<const std::byte>, ...).
  HttpRequest&& body(std::span<const std::byte> body,
                     std::string_view contentType = http::ContentTypeApplicationOctetStream) && {
    return std::move(
        this->body(std::string_view{reinterpret_cast<const char*>(body.data()), body.size()}, contentType));
  }

  // Same as body(std::string_view body, ...) but with a C-string for the body.
  HttpRequest& body(const char* body, std::string_view contentType = http::ContentTypeTextPlain) & {
    return this->body(body == nullptr ? std::string_view() : std::string_view(body), contentType);
  }

  // Rvalue overload of body(const char*, ...).
  HttpRequest&& body(const char* body, std::string_view contentType = http::ContentTypeTextPlain) && {
    return std::move(this->body(body, contentType));
  }

  // Capture the body to avoid a copy.
  // Requires an rvalue reference to avoid accidental copies of std::string.
  // The body is simply moved into this HttpMessage without any copy until the transport layer (if no compression
  // happens). Empty body is allowed - this will remove any existing body. The content type must be valid. Defaults to
  // "text/plain".
  // It is possible to call 'bodyAppend()' on the moved std::string - this will call std::string::append() on the
  // captured std::string.
  HttpRequest& body(std::string&& body, std::string_view contentType = http::ContentTypeTextPlain) & {
    HttpMessage::body(std::move(body), contentType);
    return *this;
  }

  // Rvalue overload of body(std::string&&, ...).
  HttpRequest&& body(std::string&& body, std::string_view contentType = http::ContentTypeTextPlain) && {
    return std::move(this->body(std::move(body), contentType));
  }

  // Same as above, but with a vector of char for the body, and 'application/octet-stream' as the default content type.
  HttpRequest& body(std::vector<char>&& body,
                    std::string_view contentType = http::ContentTypeApplicationOctetStream) & {
    HttpMessage::body(std::move(body), contentType);
    return *this;
  }

  // Rvalue overload of body(std::vector<char>&&, ...).
  HttpRequest&& body(std::vector<char>&& body,
                     std::string_view contentType = http::ContentTypeApplicationOctetStream) && {
    return std::move(this->body(std::move(body), contentType));
  }

  // Same as above, but with a vector of byte for the body, and 'application/octet-stream' as the default content type.
  HttpRequest& body(std::vector<std::byte>&& body,
                    std::string_view contentType = http::ContentTypeApplicationOctetStream) & {
    HttpMessage::body(std::move(body), contentType);
    return *this;
  }

  // Rvalue overload of body(std::vector<char>&&, ...).
  HttpRequest&& body(std::vector<std::byte>&& body,
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
  HttpRequest& body(std::unique_ptr<char[]> body, std::size_t size,
                    std::string_view contentType = http::ContentTypeApplicationOctetStream) & {
    HttpMessage::body(std::move(body), size, contentType);
    return *this;
  }

  // Rvalue overload of body(std::unique_ptr<char[]>&&, ...).
  HttpRequest&& body(std::unique_ptr<char[]> body, std::size_t size,
                     std::string_view contentType = http::ContentTypeApplicationOctetStream) && {
    return std::move(this->body(std::move(body), size, contentType));
  }

  // Same as body(std::unique_ptr<char[]>, ...), but with a unique_ptr to a byte array for the body.
  HttpRequest& body(std::unique_ptr<std::byte[]> body, std::size_t size,
                    std::string_view contentType = http::ContentTypeApplicationOctetStream) & {
    HttpMessage::body(std::move(body), size, contentType);
    return *this;
  }

  // Rvalue overload of body(std::unique_ptr<std::byte[]>&&, ...).
  HttpRequest&& body(std::unique_ptr<std::byte[]> body, std::size_t size,
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
  HttpRequest& bodyJson(const T& obj) & {
    HttpMessage::bodyJson(obj);
    return *this;
  }

  /// Rvalue overload of bodyJson. See bodyJson(const T&) & and include <aeronet/http-json.hpp>.
  template <class T>
  HttpRequest&& bodyJson(const T& obj) && {
    return std::move(bodyJson(obj));
  }

  /// Serialize 'obj' as YAML directly into the response body (Content-Type: text/yaml).
  /// Avoids intermediate copies: Glaze writes into a std::string which is then moved into the body.
  /// Throws std::runtime_error on serialization failure (e.g. from a faulty custom Glaze serializer).
  /// Definition lives in <aeronet/http-json.hpp> (include it to use this method).
  template <class T>
  HttpRequest& bodyYaml(const T& obj) & {
    HttpMessage::bodyYaml(obj);
    return *this;
  }

  /// Rvalue overload of bodyYaml. See bodyYaml(const T&) & and include <aeronet/http-json.hpp>.
  template <class T>
  HttpRequest&& bodyYaml(const T& obj) && {
    return std::move(bodyYaml(obj));
  }
#endif

  // Sets the body of this HttpRequest to point to a static buffer.
  // This can be useful for large static content like HTML pages, images, etc. that are known at compile time and have a
  // lifetime that exceeds the HttpRequest, until its data is conveyed to the transport layer.
  // Internally, this will capture the provided std::string_view.
  // Note that if bodyAppend() is called after bodyStatic(), aeronet will automatically allocate a buffer.
  HttpRequest& bodyStatic(std::string_view staticBody, std::string_view contentType = http::ContentTypeTextPlain) & {
    HttpMessage::bodyStatic(staticBody, contentType);
    return *this;
  }

  // Rvalue overload for string_view-based static body.
  HttpRequest&& bodyStatic(std::string_view staticBody, std::string_view contentType = http::ContentTypeTextPlain) && {
    return std::move(this->bodyStatic(staticBody, contentType));
  }

  // Same as string_view-based bodyStatic, but accepts a span of bytes, and defaults content type to
  // 'application/octet-stream' if not specified.
  HttpRequest& bodyStatic(std::span<const std::byte> staticBody,
                          std::string_view contentType = http::ContentTypeApplicationOctetStream) & {
    return this->bodyStatic(std::string_view{reinterpret_cast<const char*>(staticBody.data()), staticBody.size()},
                            contentType);
  }

  // Rvalue overload for span-based static body.
  HttpRequest&& bodyStatic(std::span<const std::byte> staticBody,
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
  HttpRequest& bodyAppend(std::string_view body, std::string_view contentType = {}) & {
    HttpMessage::bodyAppend(body, contentType);
    return *this;
  }

  // Rvalue overload of string_view-based bodyAppend.
  HttpRequest&& bodyAppend(std::string_view body, std::string_view contentType = {}) && {
    return std::move(this->bodyAppend(body, contentType));
  }

  // Same as string_view-based append, but accepts a span of bytes, and defaults content type to
  // 'application/octet-stream' if not specified and body is non-empty.
  HttpRequest& bodyAppend(std::span<const std::byte> body, std::string_view contentType = {}) & {
    HttpMessage::bodyAppend(body, contentType);
    return *this;
  }

  // Rvalue overload for span-based append.
  HttpRequest&& bodyAppend(std::span<const std::byte> body, std::string_view contentType = {}) && {
    return std::move(bodyAppend(body, contentType));
  }

  // Same as string_view-based append, but accepts a C-string (it should be null-terminated).
  // If the pointer is nullptr, it is treated as an empty chunk to append.
  HttpRequest& bodyAppend(const char* body, std::string_view contentType = {}) & {
    return bodyAppend(body == nullptr ? std::string_view() : std::string_view(body), contentType);
  }

  // Rvalue overload for C-string append.
  HttpRequest&& bodyAppend(const char* body, std::string_view contentType = {}) && {
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
  HttpRequest& bodyInlineSet(std::size_t maxLen, Writer&& writer, std::string_view contentType = {}) & {
    HttpMessage::bodyInlineSet(maxLen, std::forward<Writer>(writer), contentType);
    return *this;
  }

  // Rvalue overload for bodyInlineSet.
  template <class Writer>
  HttpRequest&& bodyInlineSet(std::size_t maxLen, Writer&& writer, std::string_view contentType = {}) && {
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
  HttpRequest& bodyInlineAppend(std::size_t maxLen, Writer&& writer, std::string_view contentType = {}) & {
    HttpMessage::bodyInlineAppend(maxLen, std::forward<Writer>(writer), contentType);
    return *this;
  }

  // Rvalue overload that accepts a `std::byte*` writer.
  template <class Writer>
  HttpRequest&& bodyInlineAppend(std::size_t maxLen, Writer&& writer, std::string_view contentType = {}) && {
    return std::move(bodyInlineAppend(maxLen, std::forward<Writer>(writer), contentType));
  }

  // Stream the contents of an already-open file as the request body.
  // This methods takes ownership of the 'file' object into the request and sends the entire file.
  // Notes:
  //   - file should be opened (`file` must be true)
  //   - Trailers are NOT permitted when using file
  //   - Errors: filesystem read/write errors are surfaced during transmission; callers should expect the connection
  //     to be closed on fatal I/O failures.
  //   - Content Type header: if non-empty, sets given content type value. Otherwise, attempt to guess it from the
  //     file object. If the MIME type is unknown, sets 'application/octet-stream' as Content type.
  HttpRequest& file(File fileObj, std::string_view contentType = {}) & {
    HttpMessage::file(std::move(fileObj), 0, 0, contentType);
    return *this;
  }

  // RValue overload of file(File, ...).
  HttpRequest&& file(File fileObj, std::string_view contentType = {}) && {
    return std::move(file(std::move(fileObj), 0, 0, contentType));
  }

  // Same as above, but with specified offset and length for the file content to be sent. If length is 0, it means
  // "until the end of the file". So to clear the file (or body) payload, use body("") instead.
  HttpRequest& file(File fileObj, std::size_t offset, std::size_t length, std::string_view contentType = {}) & {
    HttpMessage::file(std::move(fileObj), offset, length, contentType);
    return *this;
  }

  // Rvalue overload of file(fileObj, offset, length).
  HttpRequest&& file(File fileObj, std::size_t offset, std::size_t length, std::string_view contentType = {}) && {
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
  HttpRequest& trailerAddLine(std::string_view name, std::string_view value) & {
    HttpMessage::trailerAddLine(name, value);
    return *this;
  }

  // Adds a trailer header to be sent after the response body (RFC 7230 §4.1.2).
  HttpRequest&& trailerAddLine(std::string_view name, std::string_view value) && {
    return std::move(trailerAddLine(name, value));
  }

  // Convenient overload adding a trailer whose value is numeric.
  HttpRequest& trailerAddLine(std::string_view key, std::integral auto value) & {
    HttpMessage::trailerAddLine(key, value);
    return *this;
  }

  // Convenient overload adding a trailer whose value is numeric.
  HttpRequest&& trailerAddLine(std::string_view key, std::integral auto value) && {
    return std::move(trailerAddLine(key, value));
  }

 private:
  friend class internal::ClientConnection;
  friend class HttpClient;
  friend class HttpRequestView;
  friend class HttpResponseWriter;  // streaming writer needs access to finalize
  friend class SingleHttpServer;
  friend class StaticFileHandler;
  friend class internal::Http1WriterTransport;  // HTTP/1.1 transport for streaming
  friend class HttpRequestTest;

  // Private constructor to avoid allocating memory for the data buffer when not needed immediately.
  // Use with care! All setters currently take the assumption that the internal buffer is allocated.
  explicit constexpr HttpRequest([[maybe_unused]] HttpMessage::Check check) noexcept : HttpMessage(check) {}

  HttpRequest(http::Method method, std::string_view url, std::string_view concatenatedHeaders, Options opts);

  HttpRequest(std::size_t additionalCapacity, http::Method method, std::string_view url,
              std::string_view concatenatedHeaders, Options opts, std::string_view body, std::string_view contentType);

  // Get the current method length stored in this HttpRequest.
  [[nodiscard]] uint8_t methodLen() const noexcept {
    switch (_data[_originKeyLen]) {
      case 'G':
        return 3U;
      case 'H':
        return 4U;
      case 'P':
        switch (_data[_originKeyLen + 1]) {
          case 'O':
            return 4U;
          case 'U':
            return 3U;
          default:
            return 5U;
        }
      case 'D':
        return 6U;
      case 'C':
      case 'O':
        return 7U;
      default:
        return 5U;
    }
  }

  [[nodiscard]] char* targetBeg() noexcept { return _data.data() + _originKeyLen + methodLen() + 1; }
  [[nodiscard]] const char* targetBeg() const noexcept { return _data.data() + _originKeyLen + methodLen() + 1; }

  [[nodiscard]] const char* targetEnd() const noexcept {
    return _data.data() + headersStartPos() - http::HTTP10Sv.size() - 1U;
  }

  const char* setNewUrl(const internal::UrlParseResult& res);

  bool resolveRedirect(std::string_view location);

  // RAII helper returned by hostCStr(): guarantees a null-terminated host C-string for its lifetime
  // by temporarily overwriting the ':' separator that follows the host with '\0', restoring it on
  // destruction (the same trick as CharReplacer in tcp-connector.cpp). While alive, the backing
  // buffer is transiently mutated, so originKey()/port-string must not be read concurrently.
  class HostCStr {
   public:
    HostCStr(char* host, std::size_t hostLen) : _sep(host + hostLen), _saved(*_sep), _host(host) { *_sep = '\0'; }

    HostCStr(const HostCStr&) = delete;
    HostCStr(HostCStr&&) = delete;
    HostCStr& operator=(const HostCStr&) = delete;
    HostCStr& operator=(HostCStr&&) = delete;

    ~HostCStr() { *_sep = _saved; }

    // Returns a null-terminated host C-string, valid for the lifetime of the guard.
    [[nodiscard]] const char* c_str() const noexcept { return _host; }

   private:
    char* _sep;
    char _saved;
    const char* _host;
  };

  // Get a null-terminated host C-string for the lifetime of the returned RAII object, without port.
  [[nodiscard]] HostCStr hostCStr() const noexcept { return {const_cast<char*>(host().data()), _hostLen}; }

  [[nodiscard]] HttpRequest finalizeHeadersAndBody(internal::HttpClientCodec& clientCodec,
                                                   const DecompressionConfig& decompressionConfig) const;

  [[nodiscard]] HttpRequest finalizeTrailersForHttp11(std::size_t minCapturedBodySize) const {
    HttpRequest copy = clone();
    if (!copy.hasChunkedTransferEncoding()) {
      copy.HttpMessage::finalizeForHttp1(http::HTTP_1_1, copy._opts, nullptr, minCapturedBodySize);
    }
    return copy;
  }

  [[nodiscard]] std::string_view completeRequestForHttp11() const {
    return {_data.data() + _originKeyLen, _data.end()};
  }

  [[nodiscard]] std::string_view capturedPayloadForHttp11() const noexcept { return _payloadVariant.view(); }

  [[nodiscard]] HttpRequest clone() const;

  // URL data - will be set at construction time and cannot be modified.
  uint16_t _hostLen;       // length of host
  uint16_t _port;          // parsed port
  uint32_t _originKeyLen;  // length of "scheme://host:port" == offset of target
};

}  // namespace aeronet
