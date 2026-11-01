#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "aeronet/http-response.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/response-body-sink.hpp"

namespace aeronet {

struct DecompressionConfig;
struct DecompressionState;
namespace internal {
class ClientConnection;
}  // namespace internal

// Incremental, allocation-light HTTP/1.x response parser that populates an HttpResponse.
//
// The caller accumulates raw bytes in a contiguous buffer and repeatedly calls parse() with the
// full accumulated view plus an eof flag. Status line + non-reserved headers are written into the
// HttpResponse as they are parsed; the decoded body (chunked already de-framed) is installed at completion.
// Suitably-sized scratch allocations are transferred directly into the response.
//
// Status, header, chunk-metadata and trailer lines require CRLF framing, matching the server parser.
//
// Only Content-Type, Content-Length and Transfer-Encoding are consumed locally rather than stored:
// HttpResponse reconstructs Content-Type and the decoded Content-Length via body(), and chunked framing is
// de-framed away. Connection is additionally inspected for keep-alive decisions but is still stored. Other
// header values are preserved while field names are normalized to lower-case in the receive buffer before insertion.
class Http1ResponseParser {
 public:
  enum class Status : uint8_t {
    NeedMore,      // not enough bytes yet (read more, unless eof)
    Complete,      // a full response was parsed into the HttpResponse
    HeadComplete,  // (reset with streamHead only) the head of the final response is parsed and a body follows
    BodyPiece,     // (streaming only) a piece of the body is parsed: streamedBody() returns it until the next parse()
    Error,         // malformed response, or body that cannot be decoded
    Truncated,     // eof before the response was complete
    TooLarge,      // maxResponseBytes exceeded, or decoding exceeded the decompression limits
  };

  // `bodyBuf` is the reassembly buffer for chunked bodies: a whole buffered one, or the small chunks of a streamed one
  // gathered within a read (Length / UntilClose bodies stay contiguous in the caller's receive buffer and never touch
  // it). It is borrowed, not owned: a suitably-sized completed body is transferred into the response and replaced
  // with an equal-capacity empty allocation; an oversized scratch is instead retained while the small body is copied.
  // It must outlive the parser and stay distinct from the receive buffer fed to parse() (de-framing reads from that
  // buffer while writing into this one).
  explicit Http1ResponseParser(RawChars& bodyBuf) noexcept : _bodyBuf(bodyBuf) {}

  // Optional automatic response-body decompression. When `state` is non-null and the response carries a
  // (non-identity) Content-Encoding, the body is decoded at install time directly from the receive buffer
  // into `out` (with `tmp` as ping-pong scratch for stacked encodings) and the Content-Encoding header is
  // dropped. All pointers are borrowed and must outlive the parse() call(s).
  struct DecodeContext {
    DecompressionState* pState{nullptr};
    const DecompressionConfig* pConfig{nullptr};
    RawChars* pOut{nullptr};
    RawChars* pTmp{nullptr};
  };

  // Install (or clear) the decompression context used by subsequent parse() calls.
  void setDecodeContext(const DecodeContext& decodeContext) noexcept { _decode = decodeContext; }

  // Prepare for a new response. headRequest suppresses body parsing (HEAD has no body). streamHead is for a response
  // going to a sink: parse() then returns HeadComplete once the head of a final response announcing a body is parsed,
  // so that the caller can choose to stream the body (startStreaming()) or keep parsing it in the buffer as usual, and
  // a body received whole is not installed in the response (see bufferedBody()).
  void reset(bool headRequest, bool streamHead = false) noexcept;

  // Parse as much as possible. Header names are normalized in-place in `buffer`. On Complete, `resp` is fully
  // populated (without body with streamHead).
  Status parse(std::span<char> buffer, bool eof, HttpResponse& resp, std::size_t maxResponseBytes);

  // RawChars overload that permits a completed identity body occupying the initialized suffix of `buffer` to
  // transfer that allocation into the response. The buffer receives an equal-capacity empty replacement.
  Status parse(RawChars& buffer, bool eof, HttpResponse& resp, std::size_t maxResponseBytes);

  // Whether the parsed response permits connection reuse (keep-alive and bounded framing).
  [[nodiscard]] bool keepAlive() const noexcept { return _keepAlive; }

  // Bytes consumed from the front of the buffer once Complete.
  [[nodiscard]] std::size_t consumed() const noexcept { return _pos; }

  // Raw Content-Type value of the response head held in `buffer` (empty when absent). Only valid until the first
  // discardStreamed() call.
  [[nodiscard]] std::string_view contentType(std::string_view buffer) const noexcept {
    return buffer.substr(_contentTypeOff, _contentTypeLen);
  }

  // After HeadComplete, before the next parse() call: size of the body given by Content-Length, or
  // ResponseBodyInfo::kUnknownSize for a chunked body or one delimited by the closing of the connection.
  [[nodiscard]] std::size_t announcedBodySize() const noexcept {
    return _framing == Framing::Length ? _bodyRemaining : ResponseBodyInfo::kUnknownSize;
  }

  // Smallest piece of chunk data delivered straight from the receive buffer when streaming. Smaller ones are gathered
  // and delivered together: below this size, a copy costs less than a delivery (one decoder and one sink call), see
  // BM_StreamChunkedBody in benchmarks/internal/http1-response-parse_bench.cpp.
  static constexpr std::size_t kMinDirectPieceBytes = 1024;

  // After HeadComplete: deliver the body piece by piece instead of installing it in the response. parse() then returns
  // BodyPiece for each piece of body it parses, read with streamedBody() before calling parse() again, until it returns
  // another status. Streamed bytes are not counted against maxResponseBytes, and are delivered as received (no
  // decoding).
  void startStreaming() noexcept { _streaming = true; }

  [[nodiscard]] bool streaming() const noexcept { return _streaming; }

  // (reset with streamHead only) After Complete for a body that was not streamed: the body, decoded, left in the
  // scratch buffers (receive, reassembly or decoding buffer) instead of being installed in the response. Valid until
  // they are reused.
  [[nodiscard]] std::string_view bufferedBody() const noexcept { return _bufferedBody; }

  // After BodyPiece: the piece of body. It is a view into `buffer` (the one given to parse()): all the body received
  // for Length / UntilClose framing, a chunk (or the part of it received) of at least kMinDirectPieceBytes for chunked
  // framing. Smaller consecutive chunks of a read are gathered in the reassembly buffer, and delivered as one piece.
  [[nodiscard]] std::string_view streamedBody(std::string_view buffer) const noexcept {
    return _gatheredPiece ? std::string_view(_bodyBuf) : buffer.substr(_bodyStart, _pos - _bodyStart);
  }

  // Erase the bytes consumed so far from `buffer` (head and streamed body), so that the receive buffer only keeps the
  // unparsed tail (streaming only, once parse() returned another status than BodyPiece).
  void discardStreamed(RawChars& buffer) noexcept;

 private:
  friend class internal::ClientConnection;

  enum class State : uint8_t {
    StatusLine,
    Headers,
    BodyLength,
    BodyChunkSize,
    BodyChunkData,
    BodyChunkCrlf,
    BodyChunkTrailers,
    BodyUntilClose,
    Done,
  };
  enum class Framing : uint8_t { None, Length, Chunked, UntilClose };

  Status decideFraming();
  Status parseBody(std::string_view buffer, bool eof, std::size_t maxResponseBytes);
  Status parseChunkedBody(std::string_view buffer, bool eof, std::size_t maxResponseBytes);
  // Consume the next `size` bytes as a piece of a streamed body.
  Status bodyPiece(std::size_t size) noexcept {
    _bodyStart = _pos;
    _pos += size;
    return Status::BodyPiece;
  }
  // Status of a line left without CRLF at eof: Error when it holds a bare LF, Truncated otherwise.
  static Status EofInLine(std::string_view partialLine) noexcept {
    return partialLine.contains('\n') ? Status::Error : Status::Truncated;
  }

  // Return candidate when body covers exactly its initialized bytes, otherwise nullptr.
  static RawChars* WholeBufferOwner(std::string_view body, RawChars& candidate) noexcept {
    return body.size() == candidate.size() && body.data() == candidate.data() ? &candidate : nullptr;
  }

  // Install body by rotating a suitably-sized owner allocation into the response. A missing or oversized
  // owner falls back to a copy, retaining oversized scratch for later reuse. `body` may be the complete
  // initialized owner or a suffix ending at owner->end().
  static void InstallBodyStorage(HttpResponse& resp, std::string_view body, std::string_view contentType,
                                 RawChars* owner);
  // Install the (de-framed) body into `resp` (or keep it for a sink, see bufferedBody()), applying automatic
  // decompression when configured. Returns Status::Complete on success, Status::Error if decompression failed, or
  // Status::TooLarge when it exceeded the decompression limits.
  Status installBody(HttpResponse& resp, std::string_view buffer);

  // Borrowed decoded-body accumulator (see the constructor). Only used for chunked framing, where the body is
  // interleaved with chunk metadata and must be reassembled (when streaming, only the small chunks are gathered). For
  // Length / UntilClose framing the body is already contiguous in the caller's receive buffer. The RawChars parse
  // overload can transfer that allocation.
  RawChars& _bodyBuf;
  RawChars* _receiveBuf{nullptr};
  std::string_view _bufferedBody;  // see bufferedBody()
  std::size_t _pos{0};             // absolute cursor into the accumulated buffer
  std::size_t _bodyStart{0};       // offset of the first body byte (Length / UntilClose), or of the streamed piece
  std::size_t _contentTypeOff{0};  // Content-Type value position within the accumulated buffer ...
  std::size_t _contentTypeLen{0};  // ... and its length (0 => header absent). Avoids copying the value.
  std::size_t _bodyRemaining{0};   // bytes remaining for Length framing / current chunk
  DecodeContext _decode{};
  http::StatusCode _statusCode{0};
  Framing _framing{Framing::None};
  State _state{State::StatusLine};
  bool _headRequest{false};
  bool _streamHead{false};
  bool _streaming{false};
  bool _gatheredPiece{false};  // the last BodyPiece is the gathered small chunks, in the reassembly buffer
  bool _keepAlive{false};
  bool _http11{false};
  bool _connectionCloseSeen{false};
  bool _connectionKeepAliveSeen{false};
  bool _hasContentLength{false};
  bool _hasTransferEncoding{false};
  bool _chunked{false};  // the final transfer coding is chunked
};

}  // namespace aeronet
