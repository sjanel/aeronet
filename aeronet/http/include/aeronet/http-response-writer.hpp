#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>

#include "aeronet/encoder.hpp"
#include "aeronet/encoding.hpp"
#include "aeronet/file.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-message.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/lower-ascii-key.hpp"
#include "aeronet/raw-chars.hpp"

namespace aeronet {

namespace internal {
class Http1Pipeline;
}  // namespace internal

class HttpRequestView;
class CorsPolicy;
struct CompressionConfig;
class IWriterTransport;
struct CompressionState;

// HttpResponseWriter
//  - Used to stream HTTP responses in a backpressure-aware manner.
//  - Not thread-safe; intended for use within streaming path handlers only.
//  - Supports both chunked transfer encoding (default) and fixed Content-Length mode.
//  - Supports setting status, reason, headers before any body data is sent.
//  - Protocol-agnostic: delegates transport operations (framing, sending) to an IWriterTransport.
class HttpResponseWriter {
 public:
  // Get the current status code stored in this HttpResponseWriter.
  [[nodiscard]] http::StatusCode status() const { return _statusCode; }

  // Replaces the status code. Must be a 3 digits integer.
  void status(http::StatusCode code);

  // Sets or replace the reason phrase for this instance.
  // Inserting empty reason is allowed. Throws std::invalid_argument if the reason contains CR, LF or another character
  // not allowed in a header value.
  // If the data to be inserted references internal instance memory, the behavior is undefined.
  void reason(std::string_view reason);

  // Append a header line (duplicates allowed, fastest path). The name must be lower-case; literals are checked at
  // compile time through LowerAsciiKey.
  // No scan over existing headers. Prefer this when duplicates are OK or when constructing headers once.
  // A reserved header (for which IsReservedResponseHeader is true) throws std::invalid_argument: the writer sets the
  // framing (contentLength(), chunked), contentType() the Content-Type, and the server the other ones.
  // If the data to be inserted references internal instance memory, the behavior is undefined.
  void headerAddLine(LowerAsciiKey name, std::string_view value);

  // Set or replace a header value ensuring at most one instance.
  // Performs a case-sensitive linear scan (slower than headerAddLine()) over normalized lower-case header names.
  // If not found, falls back to headerAddLine(). Use only when you must guarantee uniqueness; otherwise prefer
  // headerAddLine().
  // Throws std::invalid_argument if the header name or value is invalid, or for a reserved header (as headerAddLine).
  // If the data to be inserted references internal instance memory, the behavior is undefined.
  void header(LowerAsciiKey name, std::string_view value);

  // Inserts or replaces the Content-Type header value.
  // If the data to be inserted references internal instance memory, the behavior is undefined.
  void contentType(std::string_view ct);

  // Inserts or replaces the Content-Encoding header.
  // If the data to be inserted references internal instance memory, the behavior is undefined.
  void contentEncoding(std::string_view ce) { header(http::ContentEncoding, ce); }

  // Declare an explicit fixed Content-Length for the streaming response and disable chunked framing.
  // Usage & semantics:
  // - Optional. If you never call contentLength(), the writer will default to Transfer-Encoding: chunked for
  //   non-HEAD requests (allowing indefinite / unknown-length streaming), and HEAD responses carry no Content-Length
  //   (the length of the GET response is not known). An HTTP/1.0 client does not understand chunked encoding: its
  //   response body is then delimited by the close of the connection instead.
  // - Call only if you know the exact number of body bytes that you will write. With your own Content-Encoding, it is
  //   the length of that encoded payload.
  // - Automatic compression (compression enabled, no Content-Encoding set by you) is decided from the declared
  //   length, without buffering: below CompressionConfig::minBytes, or for a content type that is not compressed, the
  //   response is sent with this Content-Length. Otherwise, the length of the compressed body is not known when the
  //   headers are sent: the declared length is dropped and the response is chunked, as without contentLength(). A
  //   HEAD response takes the same decision, whatever the handler writes for it.
  // - Precondition (only if you decide to use it): call before any body data is written (i.e. before the first
  //   write()) and before headers are sent. If you never call contentLength(), chunked mode is automatically used.
  //   Calls made after write(), end(), or once headers have been flushed are ignored (a warning is logged) and
  //   the response continues in chunked mode (or without Content-Length for HEAD).
  // - Passing 0 is allowed and results in an empty fixed-length body (no chunked framing).
  // - The number of bytes written must be len (RFC 9110 §8.6), which the writer enforces: a write beyond it sends the
  //   bytes still due only, drops the rest and returns false, and end() after fewer bytes sends nothing more. Both
  //   fail the writer: the connection is closed after the bytes sent (HTTP/1.1), the stream reset (HTTP/2), so that the
  //   client neither reads extra bytes as the next response nor waits for missing ones.
  // - Once set, the writer will NOT emit a Transfer-Encoding header and will not switch back to chunked, unless the
  //   body is compressed (see above).
  void contentLength(std::size_t len);

  // Backpressure-aware body write. Returns true if accepted (queued or immediately written). Returns
  // false if a fatal error occurred or the server marked the connection for closure / overflow.
  // The content type will be 'application/octet-stream' if not previously set by the user.
  bool writeBody(std::string_view data);

  // Backpressure-aware body write. Returns true if accepted (queued or immediately written). Returns
  // false if a fatal error occurred or the server marked the connection for closure / overflow.
  // The content type will be 'application/octet-stream' if not previously set by the user.
  bool writeBody(std::span<const std::byte> data) {
    return writeBody(std::string_view{reinterpret_cast<const char*>(data.data()), data.size()});
  }

  // Stream the given file as the response body; zero-copy where transport allows.
  // Call before headers are sent and finish with end().
  // Returns true on success, false if the writer is not in a state to accept a file
  // (already sent body data, not opened, etc).
  // 'contentType': if non-empty, sets given content type value. Otherwise, attempt to guess it from the file
  // object. If the MIME type is unknown, sets 'application/octet-stream' as Content type.
  bool file(File file, std::string_view contentType = {}) { return this->file(std::move(file), 0, 0, contentType); }

  // Stream the given file as the response body; zero-copy where transport allows.
  // Call before headers are sent and finish with end().
  // Returns true on success, false if the writer is not in a state to accept a file
  // (already sent body data, not opened, etc).
  // 'contentType': if non-empty, sets given content type value. Otherwise, attempt to guess it from the file
  // object. If the MIME type is unknown, sets 'application/octet-stream' as Content type.
  bool file(File file, std::uint64_t offset, std::uint64_t length, std::string_view contentType = {});

  // Adds a trailer header to be sent after the response body (RFC 7230 §4.1.2).
  //
  // For streaming responses using chunked transfer encoding, trailers are emitted after the
  // final zero-length chunk (0\r\n) when end() is called.
  //
  // IMPORTANT CONSTRAINTS:
  //   - Trailers are ONLY supported for chunked responses (the default for streaming).
  //   - If contentLength() was called (fixed-length response), or for an HTTP/1.0 client, trailers are NOT sent.
  //   - trailerAddLine() must be called BEFORE end() is called.
  //
  // Trailer semantics (per RFC 7230 §4.1.2):
  //   - Certain headers MUST NOT appear as trailers (RFC 9110 §6.5.1: Transfer-Encoding, Content-Length, Host,
  //     Authorization, Cookie, Set-Cookie...): they throw std::invalid_argument (http::IsForbiddenTrailerHeader), as an
  //     invalid name or value does.
  //   - Typical use: metadata computed during response generation (checksums, signatures, timings).
  //
  // Usage example:
  //   void handler(const HttpRequestView&, HttpResponseWriter& w) {
  //     w.status(200);
  //     w.writeBody("chunk 1");
  //     w.writeBody("chunk 2");
  //     w.trailerAddLine("x-checksum", computeChecksum());  // Computed after body
  //     w.trailerAddLine("x-processing-time-ms", "42");
  //     w.end();  // Trailers emitted here
  //   }
  //
  // Serialization:
  //   Trailers are buffered internally and emitted in end() as:
  //     0\r\n
  //     x-checksum: abc123\r\n
  //     x-processing-time-ms: 42\r\n
  //     \r\n
  // The name must be lower-case. Literals are checked at compile time through LowerAsciiKey.
  void trailerAddLine(LowerAsciiKey name, std::string_view value);

  // Finalize the streaming response.
  // Responsibilities:
  // - Triggers emission of headers if they have not been sent yet (lazy header strategy).
  // - Flushes any buffered data accumulated for delayed compression threshold decisions.
  // - If automatic compression was activated earlier, flushes the encoder with a final (finish) chunk.
  // - Emits the terminating zero-length chunk when operating in chunked mode.
  // - Marks the writer as finished; subsequent write()/end() calls are ignored (write() returns false).
  //
  // Compression interaction:
  // - If compression never activated (either disabled, suppressed by user-supplied Content-Encoding, or total bytes
  //   below the threshold), end() sends the buffered identity bytes (still honoring chunked vs fixed mode).
  // - If compression activated mid-stream, headers were already sent with the Content-Encoding; end() only flushes
  //   the encoder finalization bytes plus the last chunk (if chunked).
  //
  // Content-Length interaction:
  // - When a fixed Content-Length was declared via contentLength(), end() after fewer body bytes fails the writer (no
  //   padding is performed): the connection is closed (HTTP/1.1), the stream reset (HTTP/2). See contentLength().
  //
  // HEAD requests:
  // - You should still call end(); the headers of the GET response (declared Content-Length if any, Content-Encoding
  //   if the declared length or else the written bytes reach the compression threshold) are sent, but the body is
  //   neither compressed nor sent.
  //
  // Ordering & network I/O:
  // - After end() returns, all response bytes have been enqueued to the server's outgoing queue; they may still be
  //   in-flight on the socket asynchronously. Do not attempt further writes.
  //
  // Idempotency & safety:
  // - Multiple invocations are harmless; only the first has effect.
  void end();

  // A writer that failed is considered finished for callers (no further writes allowed).
  [[nodiscard]] bool finished() const { return _state == State::Ended || _state == State::Failed; }

  // Returns true in an error occur during the streaming flow (unrecoverable).
  [[nodiscard]] bool failed() const { return _state == State::Failed; }

 private:
#ifdef AERONET_ENABLE_HTTP2
  friend class http2::Http2ProtocolHandler;
#endif
  friend class internal::Http1Pipeline;

  /// Construct a writer with a transport backend.
  /// @param transport   Protocol-specific transport (lifetime must exceed writer).
  /// @param request     The originating HTTP request.
  /// @param encoding    Negotiated compression encoding (Encoding::none if disabled).
  /// @param compressionConfig  Compression configuration (thresholds, etc.).
  /// @param compressionState   Shared compression state (encoder pool).
  /// @param globalHeadersStr   Pre-formatted global headers string (with trailing separator).
  /// @param opts   Options for the HTTP message (e.g., whether to auto-add the Trailer header).
  HttpResponseWriter(IWriterTransport& transport, const HttpRequestView& request, Encoding encoding,
                     const CompressionConfig& compressionConfig, CompressionState& compressionState,
                     std::string_view globalHeadersStr, HttpMessage::Options opts);

  // Sends the head, when not sent yet. 'bodyWritten': called by a write of body bytes.
  void ensureHeadersSent(bool bodyWritten);

  // Whether the response, made for a client accepting no coding, would be compressed for a client accepting one (see
  // writeBody()): it is selected with Accept-Encoding too, and varies on it with addVaryAcceptEncodingToUncompressed
  // (RFC 9110 §12.5.5). Without a declared
  // length, the size of a body being written is not known: it counts as large enough.
  [[nodiscard]] bool identityVariantVaries(bool bodyWritten) const;

  // Starts the encoder of the negotiated encoding, unless the content type is not compressed.
  void startCompressionIfAllowed();

  // Called when the status code forbids any content: discards what could have been accumulated for the body.
  void dropContent();

  // Tells whether nothing was sent yet (the head is sent with the first body bytes past the compression threshold).
  [[nodiscard]] bool headersPending() const noexcept { return _state == State::Opened; }

  // Sends the part of 'data' still due for the declared Content-Length, drops the rest and fails the writer.
  bool writeBeyondDeclaredLength(std::string_view data);

  // Combine transient booleans into a single state machine to reduce memory and make transitions explicit.
  enum class State : std::uint8_t { Opened, HeadersSent, Ended, Failed };

  IWriterTransport* _transport;
  const HttpRequestView* _request;
  bool _noContent;           // HEAD request, or a status code without content (1xx, 204, 304): the body is never sent
  bool _isFile{false};       // the file payload is moved out of _fixedResponse when headers are emitted
  bool _fixedLength{false};  // headers sent with the declared Content-Length of a body written by writeBody()
  State _state{State::Opened};
  Encoding _encoding;
  // needed along with fixedResponse because once headers emitted, the buffer will be empty, and it is impossible to
  // access status code from it.
  http::StatusCode _statusCode{http::StatusCodeOK};

  // Internal fixed HttpResponse used solely for header accumulation and status/reason/body placeholder.
  // We never finalize until ensureHeadersSent(); body remains empty (streaming chunks / writes follow separately).
  HttpResponse _fixedResponse;
  // Declared Content-Length (IWriterTransport::kNoDeclaredLength: none, 0 is an empty body) until the headers are sent,
  // then the number of body bytes still due.
  std::size_t _declaredLength;
  EncoderContext* _activeEncoderCtx{nullptr};    // streaming context (owned by compression state)
  RawChars _preCompressBuffer;                   // threshold buffering before activation
  RawChars _compressedBuffer;                    // reusable output buffer for compression (never freed between chunks)
  RawChars _trailers;                            // Trailer headers buffered until end()
  const CompressionConfig* _pCompressionConfig;  // compression thresholds, etc.
  CompressionState* _pCompressionState;          // encoder pool
};

}  // namespace aeronet
