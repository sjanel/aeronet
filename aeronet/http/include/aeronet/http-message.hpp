#pragma once

#include <cassert>
#include <charconv>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
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
#include "aeronet/http-message-data.hpp"
#include "aeronet/http-payload.hpp"
#include "aeronet/http-version.hpp"
#include "aeronet/memory-utils-sv.hpp"
#include "aeronet/nchars.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/string-trim.hpp"
#include "aeronet/time-constants.hpp"

namespace aeronet {

class EncoderContext;

namespace internal {
class HttpCodec;
class Http1WriterTransport;
struct CompressionState;
}  // namespace internal

#ifdef AERONET_ENABLE_HTTP2
namespace http2 {
class Http2WriterTransport;
class Http2ProtocolHandler;
}  // namespace http2
namespace internal {
class Http2ClientEngine;
}
#endif

// Internal base class for HttpResponse and HttpRequest, providing common functionality for both request and response
// messages.
class HttpMessage {
 private:
  enum class Check : std::uint8_t { Yes, No };

  enum class BodySetContext : std::uint8_t { Inline, Captured };

 protected:
  // This is an internal base class - it should not be constructed directly.
  HttpMessage(std::size_t dataCapacity) : _data(dataCapacity) {}

 public:
  // Returns the size needed to store a body with given length and optional content type header.
  // It takes into account the required headers (Content-Type and Content-Length).
  static constexpr std::size_t BodySize(std::size_t bodyLen,
                                        std::size_t contentTypeLen = http::ContentTypeTextPlain.size()) {
    return bodyLen + http::HeaderSize(http::ContentType.size(), contentTypeLen) +
           http::HeaderSize(http::ContentLength.size(), nchars(bodyLen));
  }

  static constexpr std::size_t NeededBodyHeadersSize(std::size_t bodySize, std::size_t contentTypeSize) {
    if (bodySize == 0) {
      return 0;
    }
    return http::HeaderSize(http::ContentType.size(), contentTypeSize) +
           http::HeaderSize(http::ContentLength.size(), nchars(bodySize));
  }

  // --------/
  // GETTERS /
  // --------/

  // Checks if the given header key is present (case-insensitive search per RFC 7230).
  [[nodiscard]] bool hasHeader(std::string_view key) const noexcept;

  // Checks if this HttpMessage has a Content-Encoding header.
  [[nodiscard]] bool hasContentEncoding() const noexcept { return _opts.hasContentEncoding(); }

  // Retrieves the value of the first occurrence of the given header key (case-insensitive search per RFC 7230).
  // If the header is not found, returns std::nullopt.
  // Notes:
  //  - For HttpMessage that started direct automatic streaming compression, 'content-length' will not reflect the
  //    actual body length before the finalization.
  [[nodiscard]] std::optional<std::string_view> headerValue(std::string_view key) const noexcept;

  // Same as headerValue(), but returns an empty string_view instead of std::nullopt if the header is not found.
  // To distinguish between missing and present-but-empty header values, use headerValue().
  [[nodiscard]] std::string_view headerValueOrEmpty(std::string_view key) const noexcept;

  // Get a contiguous view of the current headers stored in this HttpMessage. Each header line is formatted as: name +
  // ": " + value + CRLF. If no headers are present, it returns an empty view.
  // For an HttpResponse, the returned view does not include the Date header, managed by aeronet.
  [[nodiscard]] std::string_view headersFlatView() const noexcept {
    return {_data.data() + headersStartPos() + http::CRLF.size(), _data.data() + bodyStartPos() - http::CRLF.size()};
  }

  // Return a non-allocating, iterable view over headers.
  // Each element is a HeaderView with name and value string_views.
  // Usage example:
  //   for (const auto &[name, value] : response.headers()) {
  //       process(name, value);
  //   }
  [[nodiscard]] HeadersView headers() const noexcept { return HeadersView(headersFlatView()); }

  // Get the total size of all headers, counting exactly one CRLF per header line (excluding final CRLF before body).
  // For an HttpResponse, the returned size will include the Date header, managed by aeronet.
  [[nodiscard]] std::size_t headersSize() const noexcept {
    return bodyStartPos() - headersStartPos() - http::DoubleCRLF.size() +
           (_opts.isHttpRequest() ? 0 : http::HeaderSize(http::Date.size(), RFC7231DateStrLen));
  }

  // Synonym for headersSize().
  [[nodiscard]] std::size_t headersLength() const noexcept { return headersSize(); }

  // Get the size of the head (status line + headers), excluding body, but with final CRLF before body.
  [[nodiscard]] std::size_t headSize() const noexcept { return bodyStartPos(); }

  // Synonym for headSize().
  [[nodiscard]] std::size_t headLength() const noexcept { return headSize(); }

  // Get a view of the current in memory body (no file) stored in this HttpMessage.
  // The returned view will be empty if there is either no body, or a file body.
  [[nodiscard]] std::string_view bodyInMemory() const noexcept;

  // Get the current file stored in this HttpMessage, or nullptr if no file is set.
  [[nodiscard]] const File* file() const noexcept;

  // Checks if this HttpMessage has a body (either inlined, captured or file).
  [[nodiscard]] bool hasBody() const noexcept { return !_payloadVariant.empty() || hasBodyInlined(); }

  // Checks if this HttpMessage has a body in memory (either internal buffer or captured, but no file).
  [[nodiscard]] bool hasBodyInMemory() const noexcept { return hasBodyCaptured() || hasBodyInlined(); }

  // Checks if this HttpMessage has an inlined body (appended to the main buffer after headers).
  [[nodiscard]] bool hasBodyInlined() const noexcept { return bodyStartPos() < _data.size(); }

  // Check if this HttpMessage has a captured body (no file).
  [[nodiscard]] bool hasBodyCaptured() const noexcept { return _payloadVariant.hasCapturedBody(); }

  // Check if this HttpMessage has a file payload.
  [[nodiscard]] bool hasBodyFile() const noexcept { return _payloadVariant.isFilePayload(); }

  // Get the length of the current body stored in this HttpMessage, if any (including file).
  [[nodiscard]] std::size_t bodyLength() const noexcept;

  // Synonym for bodyLength().
  [[nodiscard]] std::size_t bodySize() const noexcept { return bodyLength(); }

  // Get the length of the current inlined or captured (but no file) body stored in this HttpMessage.
  [[nodiscard]] std::size_t bodyInMemoryLength() const noexcept {
    return hasBodyCaptured() ? (_payloadVariant.size() - trailersSize()) : bodyInlinedLength();
  }

  // Synonym for bodyInMemoryLength().
  [[nodiscard]] std::size_t bodyInMemorySize() const noexcept { return bodyInMemoryLength(); }

  // Total size of the HttpMessage when serialized, excluding file payload size (if any).
  [[nodiscard]] std::size_t sizeInMemory() const noexcept { return _data.size() + _payloadVariant.size(); }

  // Get the current size of the internal buffer.
  [[nodiscard]] std::size_t sizeInlined() const noexcept { return _data.size(); }

  // Get the current capacity of the internal buffer.
  [[nodiscard]] std::size_t capacityInlined() const noexcept { return _data.capacity(); }

  // Get the length of the current inlined body stored in this HttpMessage.
  [[nodiscard]] std::size_t bodyInlinedLength() const noexcept {
    return _data.size() - bodyStartPos() - trailersSize();
  }

  // Synonym for bodyInlinedLength().
  [[nodiscard]] std::size_t bodyInlinedSize() const noexcept { return bodyInlinedLength(); }

  // Returns the current direct compression mode for this HttpMessage.
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

  // Get a view of the current trailers stored in this HttpMessage, starting at the first
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

  // Pre-allocate internal buffer capacity to avoid multiple allocations when building the response with headers and
  // inlined body.
  // The capacity should be enough to hold the entire response (status line, headers, body if inlined, trailers and the
  // CRLF chars) to avoid reallocations.
  void reserve(std::size_t capacity) { _data.reserve(capacity); }

 protected:
  // ---------------/
  // HEADER SETTERS /
  // ---------------/

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
  void headerAddLine(std::string_view key, std::string_view value);

  // Convenient overload adding a header whose value is numeric.
  void headerAddLine(std::string_view key, std::integral auto value) {
    char buf[std::numeric_limits<decltype(value)>::digits10 + 2];
    headerAddLine(key, std::string_view(buf, std::to_chars(buf, buf + sizeof(buf), value).ptr));
  }

  // Append 'value' to an existing header value, separated with 'sep', or call headerAddLine(key, value) if header
  // is missing. Example, from an empty HttpMessage, calling successively
  //   headerAppendValue("accept", "text/html", ", ")
  //   headerAppendValue("Accept", "application/json", ", ")
  // will produce:
  //   "accept: text/html"
  //   "accept: text/html, application/json"
  void headerAppendValue(std::string_view key, std::string_view value, std::string_view sep = ", ");

  // Convenient overload appending a numeric value.
  void headerAppendValue(std::string_view key, std::integral auto value, std::string_view sep = ", ") {
    char buf[std::numeric_limits<decltype(value)>::digits10 + 2];
    headerAppendValue(key, std::string_view(buf, std::to_chars(buf, buf + sizeof(buf), value).ptr), sep);
  }

  // Add or replace first header 'key' with 'value'.
  // Performs a linear scan (slower than headerAddLine()) using case-insensitive comparison of header names per
  // RFC 7230 (HTTP field names are case-insensitive). The original casing of the first occurrence is preserved in
  // HTTP1.x, but in HTTP/2 header names will be lowercased during serialization.
  // The header name and value must be valid per HTTP specifications.
  // As for 'headerAddLine()', do not insert any reserved header.
  void header(std::string_view key, std::string_view value);

  // Convenient overload setting a header to a numeric value.
  void header(std::string_view key, std::integral auto value) {
    char buf[std::numeric_limits<decltype(value)>::digits10 + 2];
    header(key, std::string_view(buf, std::to_chars(buf, buf + sizeof(buf), value).ptr));
  }

  // Remove the first occurrence of the header with the given key, search starting from backwards (case-insensitive
  // search per RFC 7230). If the header is not found, the HttpMessage is not modified.
  // Content-type and Content-Length headers cannot be removed, as they are managed by aeronet based on the body
  // content.
  void headerRemoveLine(std::string_view key);

  // Remove the first 'value' from the header with the given key, search starting from backwards (case-insensitive
  // search per RFC 7230). If the value is the only one for the header, the whole header line is removed. If there are
  // multiple values for the header, only the first specified value is removed (starting from the beginning) and the
  // other values are kept, according to the split made by given 'sep'. If the header or value is not found, the
  // HttpMessage is not modified. Separator must not be empty, and should be the same as the one used in
  // headerAppendValue() for the same header. The behavior is undefined if the header values can contain the separator
  // string.
  void headerRemoveValue(std::string_view key, std::string_view value, std::string_view sep = ", ");

  // -------------/
  // BODY SETTERS /
  // -------------/

  // Assigns the given body to this HttpMessage.
  // Empty body is allowed - this will remove any existing body.
  // The whole buffer is copied internally in the HttpMessage. If the body is large, prefer the capture by value of
  // body() overloads to avoid a copy (and possibly an allocation).
  // If the HttpMessage is eligible for direct compression (see directCompressionMode()), the body will be
  // compressed in-place in the internal buffer.
  // If content-type is omitted, it will be set to "text/plain" by default.
  // If the Body referencing internal memory of this HttpMessage is undefined behavior.
  void body(std::string_view body, std::string_view contentType = http::ContentTypeTextPlain) {
    setBodyHeaders(contentType, body.size(), BodySetContext::Inline);
    setBodyInternal(body);
    if (isHead()) {
      setHeadSize(body.size());
    } else {
      _payloadVariant = {};
    }
  }

  // Capture the body to avoid a copy.
  // Requires an rvalue reference to avoid accidental copies of std::string.
  // The body is simply moved into this HttpMessage without any copy until the transport layer (if no compression
  // happens). Empty body is allowed - this will remove any existing body. The content type must be valid. Defaults to
  // "text/plain".
  // It is possible to call 'bodyAppend()' on the moved std::string - this will call std::string::append() on the
  // captured std::string.
  void body(std::string&& body, std::string_view contentType = http::ContentTypeTextPlain) {
    setBodyHeaders(contentType, body.size(), BodySetContext::Captured);
    setBodyInternal(std::string_view{});
    setCapturedPayload(std::move(body));
  }

  // Same as above, but with a vector of char for the body, and 'application/octet-stream' as the default content type.
  void body(std::vector<char>&& body, std::string_view contentType = http::ContentTypeApplicationOctetStream) {
    setBodyHeaders(contentType, body.size(), BodySetContext::Captured);
    setBodyInternal(std::string_view{});
    setCapturedPayload(std::move(body));
  }

  // Same as above, but with a vector of byte for the body, and 'application/octet-stream' as the default content type.
  void body(std::vector<std::byte>&& body, std::string_view contentType = http::ContentTypeApplicationOctetStream) {
    setBodyHeaders(contentType, body.size(), BodySetContext::Captured);
    setBodyInternal(std::string_view{});
    setCapturedPayload(std::move(body));
  }

  // Same as above, but with a unique_ptr to a char array with its size, and 'application/octet-stream' as the default
  // content type.
  // The behavior is undefined if the char buffer actual size is different from the provided size.
  // The body is moved into this HttpMessage without any copy until the transport layer (if no compression happens).
  // Empty body is allowed (size=0) - this will remove any existing body. The content type must be valid. Defaults to
  // 'application/octet-stream'.
  // If 'bodyAppend()' is called after this, aeronet will automatically allocate a buffer and copy the captured body
  // into it before appending the new data.
  void body(std::unique_ptr<char[]> body, std::size_t size,
            std::string_view contentType = http::ContentTypeApplicationOctetStream) {
    setBodyHeaders(contentType, size, BodySetContext::Captured);
    setBodyInternal(std::string_view{});
    setCapturedPayload(std::move(body), size);
  }

  // Same as body(std::unique_ptr<char[]>, ...), but with a unique_ptr to a byte array for the body.
  void body(std::unique_ptr<std::byte[]> body, std::size_t size,
            std::string_view contentType = http::ContentTypeApplicationOctetStream) {
    setBodyHeaders(contentType, size, BodySetContext::Captured);
    setBodyInternal(std::string_view{});
    setCapturedPayload(std::move(body), size);
  }

  // Sets the body of this HttpMessage to point to a static buffer.
  // This can be useful for large static content like HTML pages, images, etc. that are known at compile time and have a
  // lifetime that exceeds the HttpMessage, until its data is conveyed to the transport layer.
  // Internally, this will capture the provided std::string_view.
  // Note that if bodyAppend() is called after bodyStatic(), aeronet will automatically allocate a buffer.
  void bodyStatic(std::string_view staticBody, std::string_view contentType = http::ContentTypeTextPlain) {
    setBodyHeaders(contentType, staticBody.size(), BodySetContext::Captured);
    setBodyInternal(std::string_view{});
    setCapturedPayload(staticBody);
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
  void bodyAppend(std::string_view body, std::string_view contentType = {});

  // Same as string_view-based append, but accepts a span of bytes, and defaults content type to
  // 'application/octet-stream' if not specified and body is non-empty.
  void bodyAppend(std::span<const std::byte> body, std::string_view contentType = {}) {
    if (!body.empty() && contentType.empty()) {
      contentType = http::ContentTypeApplicationOctetStream;
    }
    bodyAppend(std::string_view{reinterpret_cast<const char*>(body.data()), body.size()}, contentType);
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
  void bodyInlineSet(std::size_t maxLen, Writer&& writer, std::string_view contentType = {}) {
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

    const auto contentTypeHeaderSize = http::HeaderSize(http::ContentType.size(), contentType.size());
    const auto contentLengthHeaderSize = http::HeaderSize(http::ContentLength.size(), nchars(maxLen));

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
      _data.setSize(_data.size() - contentLengthHeaderSize - contentTypeHeaderSize - internalBodyAndTrailersLen());
      Copy(http::CRLF, _data.data() + _data.size() - http::CRLF.size());
      setBodyStartPos(_data.size());
    } else {
      // Set final size
      if (isHead()) {
        setHeadSize(written);
      } else {
        _data.setSize(static_cast<std::size_t>(insertPtr + written - _data.data()));
      }

      replaceHeaderValueNoRealloc(getContentLengthValuePtr(), written);
    }
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
  void bodyInlineAppend(std::size_t maxLen, Writer&& writer, std::string_view contentType = {}) {
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
    const auto contentTypeHeaderSize = http::HeaderSize(http::ContentType.size(), contentTypeValueSize);
    const std::size_t oldBodyLen = _payloadVariant.isSizeOnly() ? _payloadVariant.size() : internalBodyAndTrailersLen();
    const auto maxBodyLen = oldBodyLen + maxLen;
    const auto nCharsMaxBodyLen = nchars(maxBodyLen);
    const auto contentLengthHeaderSize = http::HeaderSize(http::ContentLength.size(), nCharsMaxBodyLen);

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
        _data.setSize(_data.size() - contentLengthHeaderSize - contentTypeHeaderSize);
        Copy(http::CRLF, _data.data() + _data.size() - http::CRLF.size());
        adjustBodyStartNoCheck(-static_cast<int64_t>(contentLengthHeaderSize) -
                               static_cast<int64_t>(contentTypeHeaderSize));
      } else {
        // we need to restore the previous content-length value
        replaceHeaderValueNoRealloc(getContentLengthValuePtr(), maxBodyLen - (maxLen - written));
      }
    } else {
#if defined(AERONET_ENABLE_BROTLI) || defined(AERONET_ENABLE_ZLIB) || defined(AERONET_ENABLE_ZSTD)
      if (_opts.isAutomaticDirectCompression()) {
        // during streaming compression, if the output buffer is too small,
        // encoders do NOT fail - they keep compressed data in their internal state and wait for more output space.
        written = appendEncodedInlineOrThrow(first, written);
      }
#endif
      if (isHead()) {
        setHeadSize(written + oldBodyLen);
      } else {
        _data.addSize(written);
      }
      replaceHeaderValueNoRealloc(getContentLengthValuePtr(), maxBodyLen - (maxLen - written));
    }
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
  void file(File fileObj, std::string_view contentType = {}) { file(std::move(fileObj), 0, 0, contentType); }

  // Same as above, but with specified offset and length for the file content to be sent. If length is 0, it means
  // "until the end of the file". So to clear the file (or body) payload, use body("") instead.
  void file(File fileObj, std::size_t offset, std::size_t length, std::string_view contentType = {});

  void trailerAddLine(std::string_view name, std::string_view value);

  // Convenient overload adding a trailer whose value is numeric.
  void trailerAddLine(std::string_view key, std::integral auto value) {
    char buf[std::numeric_limits<decltype(value)>::digits10 + 2];
    trailerAddLine(key, std::string_view(buf, std::to_chars(buf, buf + sizeof(buf), value).ptr));
  }

#ifdef AERONET_ENABLE_GLAZE
  template <class T>
  void bodyJson(const T& obj);

  /// Serialize 'obj' as YAML directly into the response body (Content-Type: text/yaml).
  /// Avoids intermediate copies: Glaze writes into a std::string which is then moved into the body.
  /// Throws std::runtime_error on serialization failure (e.g. from a faulty custom Glaze serializer).
  /// Definition lives in <aeronet/http-json.hpp> (include it to use this method).
  template <class T>
  void bodyYaml(const T& obj);
#endif

 private:
  friend class internal::Http1WriterTransport;
  friend class HttpRequest;
  friend class HttpRequestView;
  friend class HttpResponseTest;
  friend class HttpResponse;
  friend class HttpResponseWriter;
  friend class internal::HttpCodec;
  friend class SingleHttpServer;
  friend class StaticFileHandler;
#ifdef AERONET_ENABLE_HTTP2
  friend class internal::Http2ClientEngine;
  friend class http2::Http2WriterTransport;
  friend class http2::Http2ProtocolHandler;
#endif
#ifdef AERONET_ENABLE_HTTP_CLIENT
  friend class HttpClient;
  friend class HttpRequest;
  friend class ResponseParser;
  friend class HttpRequestTest;
#endif

  // Private constructor to avoid allocating memory for the data buffer when not needed immediately.
  // Use with care! All setters currently take the assumption that the internal buffer is allocated.
  explicit constexpr HttpMessage([[maybe_unused]] Check check) noexcept {}

  [[nodiscard]] constexpr bool isHead() const noexcept { return _opts.isHeadMethod(); }

  constexpr void setHeadSize(std::size_t size) {
    // Use a non-null sentinel so MSVC debug mode doesn't assert on string_view(nullptr, size).
    _payloadVariant = HttpPayload(std::string_view(&HttpPayload::kSizeOnlySentinel, size));
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
    const char* endPtr = _data.data() + _data.size();
    return {endPtr - trailersSize(), endPtr};
  }

  [[nodiscard]] std::string_view externalTrailers() const noexcept {
    const char* last = _payloadVariant.view().data() + _payloadVariant.view().size();
    return {last - trailersSize(), last};
  }

  // Check if this HttpMessage has either no body, or an inline body stored in its internal buffer.
  [[nodiscard]] bool hasNoExternalPayload() const noexcept { return _payloadVariant.empty(); }

  [[nodiscard]] constexpr std::size_t internalBodyAndTrailersLen() const noexcept {
    return _data.size() - bodyStartPos();
  }

  void setBodyHeaders(std::string_view contentTypeValue, std::size_t newBodySize, BodySetContext context);

  void setBodyInternal(std::string_view newBody);

#ifdef AERONET_ENABLE_HTTP2
  void finalizeForHttp2();
#endif

  // Same as headersFlatView but without Content-Type and Content-Length headers.
  [[nodiscard]] std::string_view headersFlatViewWithoutCTCL() const noexcept {
    return {_data.data() + headersStartPos() + http::CRLF.size(), getContentTypeHeaderLinePtr() + http::CRLF.size()};
  }

  // Simple bitmap class to pass finalization options with strong typing and better readability (passing several bools
  // is easy to get it wrong).
  class Options {
   public:
    using BmpType = uint16_t;

    static constexpr BmpType Close = 1U << 0;
    static constexpr BmpType AddTrailerHeader = 1U << 1;
    static constexpr BmpType IsHeadMethod = 1U << 2;
    static constexpr BmpType Prepared = 1U << 3;
    static constexpr BmpType AddVaryAcceptEncoding = 1U << 4;
    static constexpr BmpType HasContentEncoding = 1U << 5;
    static constexpr BmpType AutomaticDirectCompression = 1U << 6;
    static constexpr BmpType StreamingBody = 1U << 7;
    static constexpr BmpType IsHttpRequest = 1U << 8;
    static constexpr BmpType HasProxy = 1U << 9;

    Options() noexcept = default;

#if defined(AERONET_ENABLE_BROTLI) || defined(AERONET_ENABLE_ZLIB) || defined(AERONET_ENABLE_ZSTD)
    Options(internal::CompressionState& compressionState, Encoding expectedEncoding);
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

    [[nodiscard]] constexpr bool isStreamingBody() const noexcept { return (_optionsBitmap & StreamingBody) != 0; }

    [[nodiscard]] constexpr bool isHttpRequest() const noexcept { return (_optionsBitmap & IsHttpRequest) != 0; }

#ifdef AERONET_ENABLE_HTTP_CLIENT
    [[nodiscard]] constexpr bool hasProxy() const noexcept { return (_optionsBitmap & HasProxy) != 0; }
#endif

    // Tells whether the response has been pre-configured already.
    // If it's the case, then global headers have already been applied, addTrailerHeader and headMethod options
    // are known. Close is only best effort - it may still be changed later (from not close to close).
    [[nodiscard]] constexpr bool isPrepared() const noexcept { return (_optionsBitmap & Prepared) != 0; }

    constexpr void setClose() noexcept { _optionsBitmap |= Close; }

    constexpr void resetClose() noexcept { _optionsBitmap &= static_cast<BmpType>(~Close); }

    constexpr void addTrailerHeader() noexcept { _optionsBitmap |= AddTrailerHeader; }

    constexpr void setHeadMethod() noexcept { _optionsBitmap |= IsHeadMethod; }

    constexpr void addVaryAcceptEncoding() noexcept { _optionsBitmap |= AddVaryAcceptEncoding; }

    constexpr void setHasContentEncoding() noexcept { _optionsBitmap |= HasContentEncoding; }

    constexpr void resetHasContentEncoding() noexcept { _optionsBitmap &= static_cast<BmpType>(~HasContentEncoding); }

    constexpr void setAutomaticDirectCompression() noexcept { _optionsBitmap |= AutomaticDirectCompression; }

    constexpr void resetAutomaticDirectCompression() noexcept {
      _optionsBitmap &= static_cast<BmpType>(~AutomaticDirectCompression);
    }

    // Streaming responses only ever set this (never clear it), so a set-only setter like setPrepared().
    constexpr void setStreamingBody() noexcept { _optionsBitmap |= StreamingBody; }

#ifdef AERONET_ENABLE_HTTP_CLIENT
    constexpr void setHttpRequest() noexcept { _optionsBitmap |= IsHttpRequest; }
    constexpr void setHasProxy() noexcept { _optionsBitmap |= HasProxy; }
#endif

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
    friend class HttpMessage;
    friend class HttpRequest;
    friend class HttpResponse;
    friend class internal::HttpCodec;

#if defined(AERONET_ENABLE_BROTLI) || defined(AERONET_ENABLE_ZLIB) || defined(AERONET_ENABLE_ZSTD)
    internal::CompressionState* _pCompressionState{nullptr};
#endif

    std::uint32_t _trailerLen{0};  // trailer length - no logical reason to be there, it's just to benefit from packing
    BmpType _optionsBitmap{};
    Encoding _pickedEncoding{Encoding::none};
    DirectCompressionMode _directCompressionMode{DirectCompressionMode::Off};
  };

  // Private constructor to avoid allocating memory for the data buffer when not needed immediately.
  // Use with care! All setters currently take the assumption that the internal buffer is allocated.
  explicit constexpr HttpMessage(Options opts) noexcept : _opts(opts) {}

  constexpr FilePayload* filePayloadPtr() noexcept { return _payloadVariant.getIfFilePayload(); }

  [[nodiscard]] constexpr const FilePayload* filePayloadPtr() const noexcept {
    return _payloadVariant.getIfFilePayload();
  }

  void bodyAppendUpdateHeaders(std::string_view givenContentType, std::string_view defaultContentType,
                               std::size_t totalBodyLen);

  // The headers position is stored in lower 24 bits, body pos in upper 40 bits.
  // So this means that the status line can support up to 16 MiB (which is insane and should cover all use cases), and
  // the headers can support up to 1 TiB (which is also insane and should cover all use cases).
  static constexpr std::uint32_t kHeaderPosNbBits = 24U;

  static constexpr std::uint32_t kBodyPosNbBits = (sizeof(uint64_t) * 8U) - kHeaderPosNbBits;

  static constexpr std::uint64_t kHeadersStartMask = (std::uint64_t{1} << kHeaderPosNbBits) - 1;
  static constexpr std::uint64_t kBodyStartMask = (std::uint64_t{1} << kBodyPosNbBits) - 1;

  // Returns the position of the start of the headers in the internal buffer (after the status line).
  // The position starts exactly at the first CRLF after the status line (first char returned by this method is '\r').
  // In an HttpResponse, the position returned is AFTER the built-in Date header, managed by aeronet.
  [[nodiscard]] constexpr std::uint32_t headersStartPos() const noexcept {
    return static_cast<uint32_t>(_posBitmap & kHeadersStartMask);
  }

  [[nodiscard]] constexpr std::uint64_t bodyStartPos() const noexcept {
    return (_posBitmap >> kHeaderPosNbBits) & kBodyStartMask;
  }

  constexpr void setHeadersStartPosNoCheck(std::uint64_t pos) noexcept {
    _posBitmap = (_posBitmap & (kBodyStartMask << kHeaderPosNbBits)) | pos;
  }

  // bodyStartPos covers only the message head, so overflowing 40 bits would require materializing about 1 TiB of
  // status/request line and headers in one message. Protocol limits or resource exhaustion necessarily occur first in
  // real server usage. Keep this as a Debug invariant instead of adding a Release check to every header mutation.
  constexpr void setBodyStartPos(std::uint64_t pos) noexcept {
    assert(pos <= kBodyStartMask);
    setBodyStartPosNoCheck(pos);
  }

  constexpr void setBodyStartPosNoCheck(std::uint64_t pos) noexcept {
    _posBitmap = (_posBitmap & kHeadersStartMask) | (pos << kHeaderPosNbBits);
  }

  constexpr void adjustBodyStartNoCheck(int64_t diff) noexcept {
    _posBitmap += static_cast<std::uint64_t>(diff) << kHeaderPosNbBits;
  }

  constexpr void adjustBodyStart(int64_t diff) {
    assert(diff >= -static_cast<int64_t>(bodyStartPos()) &&
           (diff < 0 || static_cast<std::uint64_t>(diff) <= kBodyStartMask - bodyStartPos()));
    adjustBodyStartNoCheck(diff);
  }

  constexpr void adjustHeadersAndBodyStart(int64_t diff) {
    // works even if diff is negative, because we cast to uint64_t first, then shift left, which is well-defined.
    _posBitmap += static_cast<std::uint64_t>(diff);
    _posBitmap += static_cast<std::uint64_t>(diff) << kHeaderPosNbBits;
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
    char* ptr = getContentLengthHeaderLinePtr() - http::HeaderSize(http::ContentType.size(), http::ContentTypeMinLen);
    for (; *ptr != '\r'; --ptr) {
    }
    return ptr;
  }

  [[nodiscard]] const char* getContentTypeHeaderLinePtr() const {
    const char* ptr =
        getContentLengthHeaderLinePtr() - http::HeaderSize(http::ContentType.size(), http::ContentTypeMinLen);
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

  char* resizeHeaderValue(char* first, std::size_t newValueLen);

  void replaceHeaderValueNoRealloc(char* first, std::string_view newValue) {
    resizeHeaderValue(first, newValue.size());
    Copy(newValue, first);
  }

  void replaceHeaderValueNoRealloc(char* first, std::size_t newValue) {
    const auto newValueLen = nchars(newValue);
    resizeHeaderValue(first, newValueLen);
    std::to_chars(first, first + newValueLen, newValue);
  }

#if defined(AERONET_ENABLE_BROTLI) || defined(AERONET_ENABLE_ZLIB) || defined(AERONET_ENABLE_ZSTD)
  // Returns the number of written bytes
  std::size_t appendEncodedInlineOrThrow(const char* pData, std::size_t sz);

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

#ifdef AERONET_ENABLE_HTTP2
  void makeAllHeaderNamesLowercaseForHttp2();
#endif

  // IMPORTANT: This method finalizes the request by appending reserved headers,
  // and returns the internal buffers stolen from this HttpMessage instance.
  // So this instance must not be used anymore after this call.
  void finalizeForHttp1(http::Version version, Options opts, const ConcatenatedHeaders* pGlobalHeaders,
                        std::size_t minCapturedBodySize);

  RawChars _data;
  // headersStartPos: position where the headers start, exactly at the first CRLF after the status line.
  // bodyStartPos: position where the body starts (immediately after CRLFCRLF).
  // Bitmap layout: [40 bits bodyStartPos][24 bits headersStartPos]
  std::uint64_t _posBitmap{};
  // Variant that can hold an external captured payload (HttpPayload).
  HttpPayload _payloadVariant;
  // When HEAD is known (prepared options), body/trailer storage can be suppressed while preserving lengths.
  Options _opts;
};

}  // namespace aeronet
