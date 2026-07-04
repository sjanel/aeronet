#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "aeronet/http-response.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/raw-chars.hpp"

namespace aeronet {

struct DecompressionConfig;
namespace internal {
struct RequestDecompressionState;
}  // namespace internal

// Incremental, allocation-light HTTP/1.x response parser that populates an HttpResponse.
//
// The caller accumulates raw bytes in a contiguous buffer and repeatedly calls parse() with the
// full accumulated view plus an eof flag. Status line + non-reserved headers are written into the
// HttpResponse as they are parsed; the decoded body (chunked already de-framed) is accumulated and
// installed via HttpResponse::body() at completion.
//
// Only Content-Type, Content-Length and Transfer-Encoding are consumed locally rather than stored
// verbatim: HttpResponse reconstructs Content-Type and the decoded Content-Length via body(), and
// chunked framing is de-framed away. Connection is additionally inspected for keep-alive decisions but
// is still stored. Every other header - including Date / TE / Trailer / Upgrade - is preserved
// losslessly via HttpResponse::rawHeader().
class ResponseParser {
 public:
  enum class Status : uint8_t {
    NeedMore,  // not enough bytes yet (read more, unless eof)
    Complete,  // a full response was parsed into the HttpResponse
    Error      // malformed response / limit exceeded
  };

  // `bodyBuf` is the reassembly buffer for chunked bodies (Length / UntilClose bodies stay contiguous in
  // the caller's receive buffer and never touch it). It is borrowed, not owned: the caller supplies a
  // reusable buffer (HttpClient::bodyBuffer()) so its allocation persists across exchanges instead of being
  // freed and re-grown per response. It must outlive the parser and stay distinct from the receive buffer
  // fed to parse() (de-framing reads from that buffer while writing into this one).
  explicit ResponseParser(RawChars& bodyBuf) noexcept : _bodyBuf(&bodyBuf) {}

  // Optional automatic response-body decompression. When `state` is non-null and the response carries a
  // (non-identity) Content-Encoding, the body is decoded at install time directly from the receive buffer
  // into `out` (with `tmp` as ping-pong scratch for stacked encodings) and the Content-Encoding header is
  // dropped. All pointers are borrowed and must outlive the parse() call(s).
  struct DecodeContext {
    internal::RequestDecompressionState* state{nullptr};
    const DecompressionConfig* config{nullptr};
    RawChars* out{nullptr};
    RawChars* tmp{nullptr};
  };

  // Install (or clear) the decompression context used by subsequent parse() calls.
  void setDecodeContext(const DecodeContext& decodeContext) noexcept { _decode = decodeContext; }

  // Prepare for a new response. headRequest suppresses body parsing (HEAD has no body).
  void reset(bool headRequest) noexcept;

  // Parse as much as possible. On Complete, `resp` is fully populated.
  Status parse(std::string_view buffer, bool eof, HttpResponse& resp, std::size_t maxResponseBytes);

  // Whether the parsed response permits connection reuse (keep-alive and bounded framing).
  [[nodiscard]] bool keepAlive() const noexcept { return _keepAlive; }

  // Bytes consumed from the front of the buffer once Complete.
  [[nodiscard]] std::size_t consumed() const noexcept { return _pos; }

 private:
  enum class State : uint8_t {
    StatusLine,
    Headers,
    BodyLength,
    BodyChunkSize,
    BodyChunkData,
    BodyChunkCrlf,
    BodyChunkTrailers,
    BodyUntilClose,
    Done
  };
  enum class Framing : uint8_t { None, Length, Chunked, UntilClose };

  Status decideFraming();
  Status parseBody(std::string_view buffer, bool eof, std::size_t maxResponseBytes);
  // Install the (de-framed) body into `resp`, applying automatic decompression when configured.
  // Returns Status::Complete on success, or Status::Error if decompression failed.
  Status installBody(HttpResponse& resp, std::string_view buffer) const;

  // Borrowed decoded-body accumulator (see the constructor). Only used for chunked framing, where the body
  // is interleaved with chunk metadata and must be reassembled. For Length / UntilClose framing the body is
  // already contiguous in the caller's receive buffer, so it is installed as a view (no second copy here).
  RawChars* _bodyBuf;
  std::size_t _pos{0};             // absolute cursor into the accumulated buffer
  std::size_t _bodyStart{0};       // absolute offset of the first body byte (Length / UntilClose framing)
  std::size_t _contentTypeOff{0};  // Content-Type value position within the accumulated buffer ...
  std::size_t _contentTypeLen{0};  // ... and its length (0 => header absent). Avoids copying the value.
  std::size_t _bodyRemaining{0};   // bytes remaining for Length framing / current chunk
  DecodeContext _decode{};
  Framing _framing{Framing::None};
  State _state{State::StatusLine};
  http::StatusCode _statusCode{0};
  bool _headRequest{false};
  bool _keepAlive{false};
  bool _http11{false};
  bool _connectionCloseSeen{false};
  bool _connectionKeepAliveSeen{false};
  bool _hasContentLength{false};
  bool _chunked{false};
};

}  // namespace aeronet
