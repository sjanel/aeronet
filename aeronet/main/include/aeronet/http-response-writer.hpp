// Streaming response writer for aeronet HTTP/1.1 (phase 1 skeleton)
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

#include "aeronet/encoding.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-status-code.hpp"
#include "encoder.hpp"
#include "file.hpp"
#include "raw-chars.hpp"

namespace aeronet {

class HttpServer;

class HttpResponseWriter {
 public:
  // Replaces the status code. Must be a 3 digits integer.
  void statusCode(http::StatusCode code);

  // Convenience overload: set both status code and reason phrase in one call.
  void statusCode(http::StatusCode code, std::string_view reason);

  // Sets or replace the reason phrase for this instance.
  // Inserting empty reason is allowed.
  // If the data to be inserted references internal instance memory, the behavior is undefined.
  void reason(std::string_view reason);

  // Append a header line (duplicates allowed, fastest path).
  // No scan over existing headers. Prefer this when duplicates are OK or
  // when constructing headers once.
  // Do not insert any reserved header (for which IsReservedResponseHeader is true), doing so is undefined behavior.
  // If the data to be inserted references internal instance memory, the behavior is undefined.
  void addCustomHeader(std::string_view name, std::string_view value);

  // Set or replace a header value ensuring at most one instance.
  // Performs a linear scan (slower than addCustomHeader()) using case-insensitive comparison of header names per
  // RFC 7230 (HTTP field names are case-insensitive). The original casing of the first occurrence is preserved.
  // If not found, falls back to addCustomHeader(). Use only when you must guarantee uniqueness; otherwise prefer
  // addCustomHeader().
  // Do not insert any reserved header (for which IsReservedResponseHeader is true), doing so is undefined behavior.
  // If the data to be inserted references internal instance memory, the behavior is undefined.
  void customHeader(std::string_view name, std::string_view value);

  // Inserts or replaces the Content-Type header.
  // If the data to be inserted references internal instance memory, the behavior is undefined.
  void contentType(std::string_view ct) { customHeader(http::ContentType, ct); }

  // Inserts or replaces the Content-Encoding header.
  // If the data to be inserted references internal instance memory, the behavior is undefined.
  void contentEncoding(std::string_view ce) { customHeader(http::ContentEncoding, ce); }

  // Declare an explicit fixed Content-Length for the streaming response and disable chunked framing.
  // Usage & semantics:
  // - Optional. If you never call contentLength(), the writer will default to Transfer-Encoding: chunked for
  //   non-HEAD requests (allowing indefinite / unknown-length streaming) and will synthesize a correct
  //   Content-Length: 0 for HEAD responses.
  // - Call only if you know the exact number of body bytes that will be sent (the on-the-wire size). This means:
  //     * If you rely on aeronet's automatic compression (no user provided Content-Encoding and compression
  //       is enabled), you SHOULD NOT call contentLength() because the final compressed size is not known
  //       ahead of time. Use chunked mode instead. (If you do call it, you must supply the compressed size
  //       you will ultimately produce; aeronet does not recalculate or adjust it.)
  //     * If you supply your own Content-Encoding (manual compression or identity) you may set the length
  //       of that encoded payload exactly.
  // - Precondition (only if you decide to use it): call before any body data is written (i.e. before the first
  //   write()) and before headers are sent. If you never call contentLength(), chunked mode is automatically used.
  //   Calls made after write(), end(), or once headers have been flushed are ignored (a debug log is emitted) and
  //   the response continues in chunked mode (or with an auto-synthesized length for HEAD).
  // - Passing 0 is allowed and results in an empty fixed-length body (no chunked framing).
  // - The library does not (yet) enforce that the number of bytes written matches len; a mismatch is a protocol
  //   error and may confuse clients (too few bytes => truncated response; too many bytes => bytes interpreted
  //   as the start of the next message / connection corruption). Future versions may add debug assertions.
  // - Once set, the writer will NOT emit a Transfer-Encoding header and will not switch back to chunked.
  void contentLength(std::size_t len);

  // Backpressure-aware body write. Returns true if accepted (queued or immediately written). Returns
  // false if a fatal error occurred or the server marked the connection for closure / overflow.
  bool writeBody(std::string_view data);

  // Stream the given file as the response body; zero-copy where transport allows.
  // Call before headers are sent and finish with end().
  void file(File fileObj, std::uint64_t offset = 0, std::uint64_t length = 0);

  // Adds a trailer header to be sent after the response body (RFC 7230 §4.1.2).
  //
  // For streaming responses using chunked transfer encoding, trailers are emitted after the
  // final zero-length chunk (0\r\n) when end() is called.
  //
  // IMPORTANT CONSTRAINTS:
  //   - Trailers are ONLY supported for chunked responses (the default for streaming).
  //   - If contentLength() was called (fixed-length response), trailers are NOT sent.
  //   - addTrailer() must be called BEFORE end() is called.
  //   - Calling addTrailer() after end() is a no-op (logged as a warning in debug builds).
  //
  // Trailer semantics (per RFC 7230 §4.1.2):
  //   - Certain headers MUST NOT appear as trailers (e.g., Transfer-Encoding, Content-Length,
  //     Host, Authorization, Cookie, Set-Cookie). No validation is performed here for performance;
  //     sending forbidden trailers is undefined behavior.
  //   - Typical use: metadata computed during response generation (checksums, signatures, timings).
  //
  // Usage example:
  //   void handler(const HttpRequest&, HttpResponseWriter& w) {
  //     w.statusCode(200);
  //     w.writeBody("chunk 1");
  //     w.writeBody("chunk 2");
  //     w.addTrailer("X-Checksum", computeChecksum());  // Computed after body
  //     w.addTrailer("X-Processing-Time-Ms", "42");
  //     w.end();  // Trailers emitted here
  //   }
  //
  // Serialization:
  //   Trailers are buffered internally and emitted in end() as:
  //     0\r\n
  //     X-Checksum: abc123\r\n
  //     X-Processing-Time-Ms: 42\r\n
  //     \r\n
  //
  // Thread safety: Not thread-safe (same as all other methods).
  void addTrailer(std::string_view name, std::string_view value);

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
  // - When a fixed Content-Length was declared via contentLength(), end() does NOT verify that the total number of
  //   body bytes written matches the declared length (no padding or truncation is performed). Mismatches are a
  //   protocol error at the application layer and may lead to client confusion; future versions may add debug checks.
  //
  // HEAD requests:
  // - You should still call end(); headers (including Content-Length and any negotiated encoding) are sent, but the
  //   body / chunks are suppressed.
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
  friend class HttpServer;

  HttpResponseWriter(HttpServer& srv, int fd, bool headRequest, bool requestConnClose, Encoding compressionFormat);

  void ensureHeadersSent();
  void emitChunk(std::string_view data);
  void emitLastChunk();

  bool enqueue(HttpResponseData httpResponseData);

  bool accumulateInPreCompressBuffer(std::string_view data);

  [[nodiscard]] bool chunked() const { return _declaredLength == 0 && !_head; }

  HttpServer* _server{nullptr};
  int _fd{-1};
  bool _head{false};
  // Combine transient booleans into a single state machine to reduce memory and
  // make transitions explicit.
  enum class State : std::uint8_t { Opened, HeadersSent, Ended, Failed };
  State _state{State::Opened};
  bool _requestConnClose{false};
  Encoding _compressionFormat{Encoding::none};
  bool _compressionActivated{false};

  // Internal fixed HttpResponse used solely for header accumulation and status/reason/body placeholder.
  // We never finalize until ensureHeadersSent(); body remains empty (streaming chunks / writes follow separately).
  HttpResponse _fixedResponse{http::StatusCodeOK};
  std::size_t _declaredLength{0};
  std::size_t _bytesWritten{0};
  std::unique_ptr<EncoderContext> _activeEncoderCtx;  // streaming context
  RawChars _preCompressBuffer;                        // threshold buffering before activation
  RawChars _trailers;                                 // Trailer headers (RFC 7230 §4.1.2) buffered until end()
};

}  // namespace aeronet
