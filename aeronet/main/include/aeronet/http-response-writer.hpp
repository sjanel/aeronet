// Streaming response writer for aeronet HTTP/1.1 (phase 1 skeleton)
#pragma once

#include <cstddef>
#include <memory>
#include <string_view>

#include "aeronet/encoding.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-status-code.hpp"
#include "encoder.hpp"
#include "raw-chars.hpp"

namespace aeronet {

class HttpServer;

class HttpResponseWriter {
 public:
  HttpResponseWriter(HttpServer& srv, int fd, bool headRequest, bool requestConnClose, Encoding compressionFormat);

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

  // Backpressure-aware write. Returns true if accepted (queued or immediately written). Returns
  // false if a fatal error occurred or the server marked the connection for closure / overflow.
  bool write(std::string_view data);

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

  [[nodiscard]] bool finished() const { return _ended; }

  // Automatic compression suppression: if user supplies a Content-Encoding header,
  // we will not perform automatic compression (user fully controls encoding). Exposed via flag.
  [[nodiscard]] bool userProvidedContentEncoding() const { return _userProvidedContentEncoding; }

 private:
  void ensureHeadersSent();
  void emitChunk(std::string_view data);
  void emitLastChunk();
  bool enqueue(std::string_view data);

  HttpServer* _server{nullptr};
  int _fd{-1};
  bool _head{false};
  bool _headersSent{false};
  bool _chunked{true};
  bool _ended{false};
  bool _failed{false};
  bool _requestConnClose{false};
  bool _userSetContentType{false};
  bool _userProvidedContentEncoding{false};
  Encoding _compressionFormat;
  bool _compressionActivated{false};
  // Internal fixed HttpResponse used solely for header accumulation and status/reason/body placeholder.
  // We never finalize until ensureHeadersSent(); body remains empty (streaming chunks / writes follow separately).
  HttpResponse _fixedResponse{200, http::ReasonOK};
  std::size_t _declaredLength{0};
  std::size_t _bytesWritten{0};
  RawChars _chunkBuf;                                 // reusable buffer for coalesced small/medium chunks
  std::unique_ptr<EncoderContext> _activeEncoderCtx;  // streaming context
  RawChars _preCompressBuffer;                        // threshold buffering before activation
};

}  // namespace aeronet
