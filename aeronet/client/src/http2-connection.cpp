// Native HTTP/2 client engine: drives a client-mode http2::Http2Connection (the same frame codec +
// HPACK bricks as the server) through HttpClient's synchronous transport / event-loop machinery.
// The whole translation unit is compiled out when HTTP/2 support is disabled.
#include "aeronet/http2-connection.hpp"

#include <algorithm>
#include <cassert>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string_view>

#include "aeronet/client-connection.hpp"
#include "aeronet/client-request.hpp"
#include "aeronet/event.hpp"
#include "aeronet/headers-view-map.hpp"
#include "aeronet/http-client-config.hpp"
#include "aeronet/http-client-error.hpp"
#include "aeronet/http-client.hpp"
#include "aeronet/http-codec.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-headers-view.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/http2-config.hpp"
#include "aeronet/http2-frame-types.hpp"
#include "aeronet/http2-frame.hpp"
#include "aeronet/http2-stream.hpp"
#include "aeronet/log.hpp"
#include "aeronet/memory-utils-sv.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/ndigits.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/simple-charconv.hpp"
#include "aeronet/string-equal-ignore-case.hpp"
#include "aeronet/timedef.hpp"
#include "aeronet/tolower-str.hpp"
#include "aeronet/transport.hpp"
#include "aeronet/url.hpp"
#include "client-request-builder.hpp"
#include "http-client-codec.hpp"

namespace aeronet::internal {

namespace {

// Transport read granularity (mirrors the HTTP/1.1 engine).
constexpr std::size_t kReadChunk = 16384;

// Upper bound on the DATA bytes encoded into the output buffer per flush while uploading a request body.
// Flow-control windows can be large (the peer may grant hundreds of megabytes); without a cap the whole
// window would be copied into the output buffer at once. 64 KiB keeps memory bounded while amortizing the
// per-frame overhead (4 full frames at the default SETTINGS_MAX_FRAME_SIZE).
constexpr std::size_t kMaxDataBytesPerFlush = 64UL * 1024UL;

// Highest client-initiated stream id (31-bit space, odd ids only -- RFC 9113 §5.1.1).
constexpr uint32_t kMaxStreamId = 0x7FFFFFFFU;

}  // namespace

// Per-connection HTTP/2 client state: the client-mode Http2Connection (HPACK tables, flow-control
// windows, stream table) plus the bookkeeping of the single in-flight exchange. It travels with the
// pooled connection so a reused connection keeps its negotiated settings and HPACK dynamic tables.
//
// The engine is driven synchronously: HttpClient runs one exchange at a time, so exactly one stream is
// in flight per exchange (the connection still multiplexes housekeeping frames -- SETTINGS, PING,
// WINDOW_UPDATE, GOAWAY -- freely). Callbacks are installed once at construction and route into the
// per-exchange state below; the engine address is stable (heap-allocated behind ClientConnection).
class Http2ClientEngine {
 public:
  // First failure observed on the current exchange (first error wins; None when the exchange is healthy).
  enum class Failure : uint8_t {
    None,
    TooBig,         // response body exceeded HttpClientConfig::maxResponseBytes
    Malformed,      // missing/invalid :status pseudo-header
    StreamReset,    // peer sent RST_STREAM for our stream
    StreamRefused,  // peer sent GOAWAY with lastStreamId below our stream (request not processed)
  };

  explicit Http2ClientEngine(const Http2Config& config) : _conn(config, /*isServer=*/false) {
    _conn.setOnHeadersDecoded([this](uint32_t streamId, const HeadersViewMap& headers, bool endStream) {
      onHeaders(streamId, headers, endStream);
    });
    _conn.setOnData([this](uint32_t streamId, std::span<const std::byte> data, bool endStream) {
      onData(streamId, data, endStream);
    });
    _conn.setOnStreamReset([this](uint32_t streamId, http2::ErrorCode errorCode) {
      if (streamId == _streamId && _resp != nullptr) {
        log::error("HTTP/2 client: stream {} reset by peer ({})", streamId, http2::ErrorCodeName(errorCode));
        setFailure(Failure::StreamReset);
      }
    });
    _conn.setOnStreamClosed([this](uint32_t streamId) {
      if (streamId == _streamId) {
        _streamClosed = true;
      }
    });
    _conn.setOnGoAway([this](uint32_t lastStreamId, http2::ErrorCode errorCode, std::string_view debugData) {
      _goAwayReceived = true;
      if (_streamId > lastStreamId && !_streamClosed && _resp != nullptr) {
        log::warn("HTTP/2 client: stream {} refused by GOAWAY ({}{}{})", _streamId, http2::ErrorCodeName(errorCode),
                  debugData.empty() ? "" : ": ", debugData);
        setFailure(Failure::StreamRefused);
      }
    });
  }

  [[nodiscard]] http2::Http2Connection& conn() noexcept { return _conn; }

  // Prepare the per-exchange state and allocate the next (odd) stream id.
  uint32_t beginExchange(HttpResponse& resp, RawChars& bodyBuf, std::size_t maxResponseBytes) noexcept {
    _resp = &resp;
    _bodyBuf = &bodyBuf;
    _maxResponseBytes = maxResponseBytes;
    _contentType.clear();
    _streamId = _nextStreamId;
    _nextStreamId += 2;
    _failure = Failure::None;
    _streamClosed = false;
    _finalHeadersSeen = false;
    _responseComplete = false;
    return _streamId;
  }

  // Detach the per-exchange state so late frames on the (now finished) stream cannot touch it.
  void endExchange() noexcept {
    _resp = nullptr;
    _bodyBuf = nullptr;
  }

  [[nodiscard]] Failure failure() const noexcept { return _failure; }
  [[nodiscard]] bool streamClosed() const noexcept { return _streamClosed; }
  // Whether the response is fully received (END_STREAM seen). The stream itself may still be open on
  // our side when the server responded before the request body was fully uploaded (RFC 9113 §8.1).
  [[nodiscard]] bool responseComplete() const noexcept { return _responseComplete || _streamClosed; }
  [[nodiscard]] std::string_view contentType() const noexcept { return _contentType; }

  [[nodiscard]] HttpClientErrc failureErrc() const noexcept {
    switch (_failure) {
      case Failure::TooBig:
        [[fallthrough]];
      case Failure::Malformed:
        return HttpClientErrc::malformedResponse;
      default:
        return HttpClientErrc::connectionClosed;  // StreamReset / StreamRefused: peer aborted the exchange
    }
  }

  // Send-side flow-control budget for `streamId`: min(stream window, connection window), never negative.
  [[nodiscard]] std::size_t sendWindow(uint32_t streamId) noexcept {
    const http2::Http2Stream* stream = _conn.getStream(streamId);
    if (stream == nullptr) {
      return 0;
    }
    return static_cast<std::size_t>(std::max(std::min(stream->sendWindow(), _conn.connectionSendWindow()), 0));
  }

  // Whether this pooled connection can host one more exchange: connection open (no GOAWAY in either
  // direction), stream-id space left, and the optional per-connection stream budget not exhausted.
  [[nodiscard]] bool reusable() const noexcept {
    if (!_conn.isOpen() || _goAwayReceived || _nextStreamId > kMaxStreamId) {
      return false;
    }
    const uint32_t maxStreams = _conn.localSettings().maxStreamsPerConnection;
    return maxStreams == 0 || (_nextStreamId - 1U) / 2U < maxStreams;
  }

  // Flush the connection's pending output to the transport, pumping the event loop on would-block. Sets
  // `requestSent` as soon as any byte reaches the transport.
  [[nodiscard]] std::expected<void, HttpClientErrc> flushOutput(HttpClient& client, ITransport& transport,
                                                                NativeHandle fd, SteadyClock::time_point deadline,
                                                                bool& requestSent) {
    while (_conn.hasPendingOutput()) {
      const std::span<const std::byte> out = _conn.getPendingOutput();
      const ITransport::TransportResult res =
          transport.write(std::string_view{reinterpret_cast<const char*>(out.data()), out.size()});
      if (res.bytesProcessed > 0) {
        _conn.onOutputWritten(res.bytesProcessed);
        requestSent = true;
        continue;
      }
      if (res.want == TransportHint::Error) {
        return std::unexpected(HttpClientErrc::writeError);
      }
      const EventBmp interest = (res.want == TransportHint::ReadReady) ? EventIn : EventOut;
      if (!client.waitIo(fd, interest, deadline)) {
        return std::unexpected(HttpClientErrc::timeout);
      }
    }
    return {};
  }

  // Feed buffered / freshly read bytes to the connection until at least one frame is consumed, pumping
  // the event loop on would-block. Frame effects surface through the callbacks; unconsumed bytes (a
  // partial frame) stay in the engine's input buffer so nothing is lost across calls -- or exchanges, on
  // a pooled connection. Bytes already buffered are re-processed before reading: processInput stops
  // early at a GOAWAY frame, so complete frames may still be waiting behind it from a previous call.
  [[nodiscard]] std::expected<void, HttpClientErrc> readAndProcess(HttpClient& client, ITransport& transport,
                                                                   NativeHandle fd, SteadyClock::time_point deadline) {
    for (;;) {
      if (!_inBuf.empty()) {
        const auto processed = _conn.processInput(std::as_bytes(std::span<const char>(_inBuf.data(), _inBuf.size())));
        using Action = http2::Http2Connection::ProcessResult::Action;
        if (processed.action == Action::Error) {
          log::error("HTTP/2 client: protocol error: {} ({})", processed.errorMessage,
                     http2::ErrorCodeName(processed.errorCode));
          return std::unexpected(HttpClientErrc::malformedResponse);
        }
        if (processed.action == Action::Closed) {
          return std::unexpected(HttpClientErrc::connectionClosed);
        }
        // Continue / OutputReady / GoAway: frames were handled (GOAWAY effects arrive via the callback).
        if (processed.bytesConsumed != 0) {
          _inBuf.erase_front(processed.bytesConsumed);
          return {};
        }
        // Only a partial frame is buffered: fall through and read more.
      }
      _inBuf.ensureAvailableCapacityExponential(kReadChunk);
      const ITransport::TransportResult res = transport.read(_inBuf.data() + _inBuf.size(), kReadChunk);
      if (res.bytesProcessed > 0) {
        _inBuf.addSize(res.bytesProcessed);
        continue;  // process what we just buffered
      }
      if (res.want == TransportHint::ReadReady) {
        if (!client.waitIo(fd, EventIn, deadline)) {
          return std::unexpected(HttpClientErrc::timeout);
        }
        continue;
      }
      if (res.want == TransportHint::WriteReady) {
        if (!client.waitIo(fd, EventOut, deadline)) {
          return std::unexpected(HttpClientErrc::timeout);
        }
        continue;
      }
      // 0 bytes, no want => orderly close before the exchange completed.
      return std::unexpected(HttpClientErrc::connectionClosed);
    }
  }

 private:
  void setFailure(Failure failure) noexcept {
    if (_failure == Failure::None) {
      _failure = failure;
    }
  }

  void onHeaders(uint32_t streamId, const HeadersViewMap& headers, bool endStream) {
    if (streamId != _streamId || _resp == nullptr) {
      return;  // foreign stream (push is disabled) or an exchange already detached: ignore
    }
    if (!_finalHeadersSeen) {
      const auto statusIt = headers.find(http::PseudoHeaderStatus);
      if (statusIt == headers.end() || statusIt->second.size() != 3) {
        log::error("HTTP/2 client: response HEADERS on stream {} without a valid :status", streamId);
        setFailure(Failure::Malformed);
        return;
      }
      // The size()==3 check above guarantees read3 stays within the view.
      // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage)
      const auto statusCode = static_cast<http::StatusCode>(read3(statusIt->second.data()));
      if (statusCode < 100) {
        setFailure(Failure::Malformed);
        return;
      }
      if (statusCode < 200) {
        return;  // 1xx interim block: skip it and await the final response headers
      }
      _resp->status(statusCode);
      _finalHeadersSeen = true;
    }
    // Regular headers (and trailers, delivered as a later block) are preserved losslessly, except
    // Content-Type and Content-Length which HttpResponse reconstructs via body() -- mirroring the
    // HTTP/1.1 response parser. The decoded views only live for this callback, so values are copied.
    for (const auto& [name, value] : headers) {
      if (name.starts_with(':')) {
        continue;
      }
      if (CaseInsensitiveEqual(name, http::ContentType)) {
        _contentType.assign(value);
        continue;
      }
      if (CaseInsensitiveEqual(name, http::ContentLength)) {
        continue;
      }
      _resp->rawHeader(name, value);
    }
    if (endStream) {
      _responseComplete = true;  // response (or its trailers block) carried END_STREAM
    }
  }

  void onData(uint32_t streamId, std::span<const std::byte> data, bool endStream) {
    if (streamId != _streamId || _bodyBuf == nullptr) {
      return;
    }
    if (endStream) {
      _responseComplete = true;
    }
    if (data.empty() || _failure != Failure::None) {
      return;
    }
    if (_bodyBuf->size() + data.size() > _maxResponseBytes) {
      log::error("HTTP/2 client: response body on stream {} exceeds maxResponseBytes", streamId);
      setFailure(Failure::TooBig);
      return;
    }
    _bodyBuf->append(std::string_view{reinterpret_cast<const char*>(data.data()), data.size()});
  }

  http2::Http2Connection _conn;
  HttpResponse* _resp{nullptr};  // response of the in-flight exchange (null between exchanges)
  RawChars* _bodyBuf{nullptr};   // borrowed body accumulator (HttpClient::responseBuffer())
  RawChars _inBuf;               // frame input accumulator; persists so a partial frame survives exchanges
  RawChars32 _contentType;       // owned copy of the response Content-Type value (views die with decode)
  std::size_t _maxResponseBytes{0};
  uint32_t _streamId{0};      // stream id of the in-flight exchange
  uint32_t _nextStreamId{1};  // next client-initiated (odd) stream id
  Failure _failure{Failure::None};
  bool _streamClosed{false};
  bool _finalHeadersSeen{false};  // a non-interim (>= 200) response HEADERS block was received
  bool _responseComplete{false};  // END_STREAM received for the response (see responseComplete())
  bool _goAwayReceived{false};
};

ClientConnection::ClientConnection(Type type) noexcept : _type(type) {}

ClientConnection::ClientConnection(const Http2Config& http2Config)
    : _h2(std::make_unique<Http2ClientEngine>(http2Config)), _type(Type::Http2) {}

ClientConnection::ClientConnection(ClientConnection&&) noexcept = default;
ClientConnection& ClientConnection::operator=(ClientConnection&&) noexcept = default;
ClientConnection::~ClientConnection() = default;

void ClientConnection::reset() noexcept {
  _h2.reset();
  _type = Type::Empty;
  _keepAlive = false;
}

bool ClientConnection::canTakeAnotherStream() const noexcept { return _type != Type::Http2 || _h2->reusable(); }

std::string_view ClientConnection::buildHeaderBlockForHttp2(HttpClient& client, const Url& url,
                                                            const ClientRequest& req, http::Method method,
                                                            bool dropBody) {
  const HttpClientConfig& config = client.config();
  RawChars& out = client.requestBuffer();
  out.clear();

  const std::string_view methodStr = http::MethodToStr(method);
  const std::string_view scheme = url.scheme();
  const std::string_view target = url.target();
  const std::string_view hostStr = url.host();
  // The host is stored unbracketed even for IPv6 literals; :authority needs the brackets back.
  const bool hostIsIpv6 = hostStr.contains(':');
  const std::string_view userAgent = config.userAgent();

  // Single scan of the user headers, then derive the injection decisions -- shared with (and identical to)
  // the HTTP/1.1 builder. An explicit Host becomes the :authority value.
  const RequestHeaderScan scan = ScanRequestHeaders(req.headers());
  const std::string_view authorityOverride = scan.host;
  const bool addUserAgent = !userAgent.empty() && !scan.hasUserAgent;
  const std::string_view acceptEncoding = ResolveAcceptEncoding(config, scan.hasAcceptEncoding);
  const bool addAcceptEncoding = !acceptEncoding.empty();

  // Optional outbound compression of a large request body (opt-in), identical policy to HTTP/1.1. On
  // success `contentEncoding` becomes non-empty and `body` points at the codec's reusable buffer.
  std::string_view contentEncoding;
  std::string_view body = maybeCompressRequestBody(client, dropBody ? std::string_view{} : req.body(),
                                                   scan.hasContentEncoding, scan.hasTransferEncoding, contentEncoding);

  // Headers a request must not carry in HTTP/2 (connection-specific, RFC 9113 §8.2.2) are dropped, as is
  // Host (carried as :authority). TE survives only as "TE: trailers". Content-Type / Content-Length are
  // dropped on a redirect rewrite to GET, and Content-Length is rewritten when the body was compressed.
  const bool compressed = !contentEncoding.empty();
  const auto skipHeader = [dropBody, compressed](std::string_view name, std::string_view value) {
    return CaseInsensitiveEqual(name, http::Host) || CaseInsensitiveEqual(name, http::Connection) ||
           CaseInsensitiveEqual(name, "keep-alive") || CaseInsensitiveEqual(name, "proxy-connection") ||
           CaseInsensitiveEqual(name, http::TransferEncoding) || CaseInsensitiveEqual(name, http::Upgrade) ||
           (CaseInsensitiveEqual(name, http::TE) && !CaseInsensitiveEqual(value, "trailers")) ||
           ((dropBody || compressed) && CaseInsensitiveEqual(name, http::ContentLength)) ||
           (dropBody && CaseInsensitiveEqual(name, http::ContentType));
  };

  const auto bodySz = body.size();
  const auto ndigitsBodySz = ndigits(bodySz);

  // First pass: exact byte count of the flat "name: value\r\n" block -- ONE allocation.
  constexpr std::size_t kLineOverhead = http::HeaderSep.size() + http::CRLF.size();
  std::size_t neededSize = http::PseudoHeaderMethod.size() + methodStr.size() + kLineOverhead +
                           http::PseudoHeaderScheme.size() + scheme.size() + kLineOverhead +
                           http::PseudoHeaderAuthority.size() + kLineOverhead + http::PseudoHeaderPath.size() +
                           target.size() + kLineOverhead;
  neededSize += authorityOverride.empty() ? AuthorityLen(url, hostIsIpv6) : authorityOverride.size();
  for (const auto& [name, value] : req.headers()) {
    if (!skipHeader(name, value)) {
      neededSize += name.size() + value.size() + kLineOverhead;
    }
  }
  if (addUserAgent) {
    neededSize += http::UserAgent.size() + userAgent.size() + kLineOverhead;
  }
  if (addAcceptEncoding) {
    neededSize += http::AcceptEncoding.size() + acceptEncoding.size() + kLineOverhead;
  }
  if (compressed) {
    neededSize += http::ContentEncoding.size() + contentEncoding.size() + kLineOverhead;
    neededSize += http::ContentLength.size() + ndigitsBodySz + kLineOverhead;
  }

  out.ensureAvailableCapacity(neededSize);
  char* pEnd = out.data();

  // Append "name: " (names below are compile-time constants, already lowercase).
  const auto appendName = [&pEnd](std::string_view name) {
    pEnd = Append(name, pEnd);
    pEnd = Append(http::HeaderSep, pEnd);
  };

  // Pseudo-headers first (mandatory order, RFC 9113 §8.3).
  appendName(http::PseudoHeaderMethod);
  pEnd = Append(methodStr, pEnd);
  pEnd = Append(http::CRLF, pEnd);

  appendName(http::PseudoHeaderScheme);
  pEnd = Append(scheme, pEnd);
  pEnd = Append(http::CRLF, pEnd);

  appendName(http::PseudoHeaderAuthority);
  pEnd = authorityOverride.empty() ? AppendAuthority(pEnd, url, hostIsIpv6) : Append(authorityOverride, pEnd);
  pEnd = Append(http::CRLF, pEnd);

  appendName(http::PseudoHeaderPath);
  pEnd = Append(target, pEnd);
  pEnd = Append(http::CRLF, pEnd);

  // User headers, names lowercased in place (HTTP/2 field names must be lowercase, RFC 9113 §8.2).
  for (const auto& [name, value] : req.headers()) {
    if (skipHeader(name, value)) {
      continue;
    }
    char* nameBeg = pEnd;
    pEnd = Append(name, pEnd);
    aeronet::tolower(nameBeg, name.size());
    pEnd = Append(http::HeaderSep, pEnd);
    pEnd = Append(value, pEnd);
    pEnd = Append(http::CRLF, pEnd);
  }
  if (addUserAgent) {
    appendName(http::UserAgent);
    pEnd = Append(userAgent, pEnd);
    pEnd = Append(http::CRLF, pEnd);
  }
  if (addAcceptEncoding) {
    appendName(http::AcceptEncoding);
    pEnd = Append(acceptEncoding, pEnd);
    pEnd = Append(http::CRLF, pEnd);
  }
  if (compressed) {
    appendName(http::ContentEncoding);
    pEnd = Append(contentEncoding, pEnd);
    pEnd = Append(http::CRLF, pEnd);
    appendName(http::ContentLength);
    pEnd = std::to_chars(pEnd, pEnd + ndigitsBodySz, bodySz).ptr;
    pEnd = Append(http::CRLF, pEnd);
  }
  out.setSize(static_cast<std::size_t>(pEnd - out.data()));
  return body;
}

HttpClientResult ClientConnection::exchangeForHttp2(HttpClient& client, ITransport& transport, NativeHandle fd,
                                                    const Url& url, const ClientRequest& req, http::Method method,
                                                    bool dropBody, SteadyClock::time_point ioDeadline,
                                                    bool& requestSent) {
  assert(_h2 != nullptr);
  Http2ClientEngine& engine = *_h2;
  http2::Http2Connection& conn = engine.conn();
  const HttpClientConfig& config = client.config();
  _keepAlive = false;

  // Fresh connection: queue the client connection preface (magic + SETTINGS + connection WINDOW_UPDATE);
  // no-op on a pooled connection. The preface bytes ride the same flush as the first HEADERS frame, and
  // the request is sent without waiting for the server's SETTINGS (RFC 9113 §3.4).
  conn.sendClientPreface();

  HttpResponse resp;
  RawChars& bodyBuf = client.responseBuffer();
  bodyBuf.clear();  // reuse the buffer's allocation across requests
  const uint32_t streamId = engine.beginExchange(resp, bodyBuf, config.maxResponseBytes);

  const std::string_view body = buildHeaderBlockForHttp2(client, url, req, method, dropBody);
  const http2::ErrorCode headersErr =
      conn.sendHeaders(streamId, http::StatusCode{}, HeadersView(client.requestBuffer()), body.empty());
  if (headersErr != http2::ErrorCode::NoError) {
    log::error("HTTP/2 client: cannot open stream {} to {} ({})", streamId, url.originKey(),
               http2::ErrorCodeName(headersErr));
    engine.endExchange();
    return std::unexpected(HttpClientErrc::connectionClosed);
  }

  // Ship the request head (plus preface / housekeeping frames), then the body under send flow control.
  // An early response (RFC 9113 §8.1: the server answers before the whole request was uploaded) stops
  // the upload; the leftover half-open stream is aborted below.
  auto ioRes = engine.flushOutput(client, transport, fd, ioDeadline, requestSent);
  std::size_t bodyOff = 0;
  while (ioRes && bodyOff < body.size() && engine.failure() == Http2ClientEngine::Failure::None &&
         !engine.responseComplete()) {
    const std::size_t window = engine.sendWindow(streamId);
    if (window == 0) {
      // Flow-control stall: pump the connection until the server grants more window.
      ioRes = engine.readAndProcess(client, transport, fd, ioDeadline);
      continue;
    }
    const std::size_t chunkSize = std::min({body.size() - bodyOff, window, kMaxDataBytesPerFlush});
    const bool endStream = bodyOff + chunkSize == body.size();
    const http2::ErrorCode dataErr =
        conn.sendData(streamId, std::as_bytes(std::span{body.data() + bodyOff, chunkSize}), endStream);
    if (dataErr != http2::ErrorCode::NoError) {
      log::error("HTTP/2 client: sending DATA on stream {} failed ({})", streamId, http2::ErrorCodeName(dataErr));
      engine.endExchange();
      return std::unexpected(HttpClientErrc::writeError);
    }
    bodyOff += chunkSize;
    ioRes = engine.flushOutput(client, transport, fd, ioDeadline, requestSent);
  }

  // Await the response: pump frames until it is fully received (or the exchange fails).
  while (ioRes) {
    if (engine.failure() != Http2ClientEngine::Failure::None) {
      engine.endExchange();
      return std::unexpected(engine.failureErrc());
    }
    if (engine.responseComplete()) {
      break;
    }
    // Flush housekeeping frames first (WINDOW_UPDATE for received DATA, SETTINGS ack) so a server
    // streaming a large response is never stalled on flow control.
    ioRes = engine.flushOutput(client, transport, fd, ioDeadline, requestSent);
    if (ioRes) {
      ioRes = engine.readAndProcess(client, transport, fd, ioDeadline);
    }
  }
  engine.endExchange();
  if (!ioRes) {
    return std::unexpected(ioRes.error());
  }

  // A stream that did not close through the frame-receive path needs explicit closure: abort a
  // partially-uploaded request with RST_STREAM(NO_ERROR) (early response, RFC 9113 §8.1), and finalize a
  // stream whose final END_STREAM went out through the send path. Both keep the pooled connection's
  // active-stream accounting exact. The per-exchange state is already detached, so the callbacks these
  // fire have nothing to touch.
  if (!engine.streamClosed()) {
    if (bodyOff < body.size()) {
      conn.sendRstStream(streamId, http2::ErrorCode::NoError);
    } else {
      conn.finalizeSendClosedStream(streamId);
    }
  }

  // Flush any last housekeeping frames so the pooled connection is clean; a failure here only affects
  // reusability, never the (already complete) response.
  bool flushedClean = true;
  if (conn.hasPendingOutput()) {
    flushedClean = static_cast<bool>(engine.flushOutput(client, transport, fd, ioDeadline, requestSent));
  }
  _keepAlive = flushedClean && engine.reusable();

  // Install the (assembled) body, applying automatic decompression -- mirrors the HTTP/1.1 parser's
  // installBody: decode straight from the accumulator into the codec's scratch buffer, drop the stale
  // Content-Encoding header, and let body() reconstruct Content-Type / Content-Length.
  const std::string_view bodyView{bodyBuf};
  if (!bodyView.empty() || !engine.contentType().empty()) {
    const std::string_view contentType =
        engine.contentType().empty() ? http::ContentTypeApplicationOctetStream : engine.contentType();
    if (config.decompression.enable && !bodyView.empty()) {
      const std::string_view respEncoding = resp.headerValueOrEmpty(http::ContentEncoding);
      if (!respEncoding.empty()) {
        std::string_view decoded;
        const auto decodeRes = HttpCodec::DecompressFullBody(client.codec().decompressionState, config.decompression,
                                                             respEncoding, bodyView, client.codec().decompressOut,
                                                             client.codec().decompressTmp, decoded);
        if (decodeRes.status != http::StatusCodeOK) {
          return std::unexpected(HttpClientErrc::malformedResponse);
        }
        resp.headerRemoveLine(http::ContentEncoding);  // body not installed yet => legal & cheap
        resp.body(decoded, contentType);
        return resp;
      }
    }
    resp.body(bodyView, contentType);
  }
  return resp;
}

}  // namespace aeronet::internal
