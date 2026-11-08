#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>

#include "aeronet/client-connection.hpp"
#include "aeronet/event.hpp"
#include "aeronet/file-payload.hpp"
#include "aeronet/file.hpp"
#include "aeronet/http-client-codec.hpp"
#include "aeronet/http-client-config.hpp"
#include "aeronet/http-client-error.hpp"
#include "aeronet/http-client.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-message.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/internal/response-streamer.hpp"
#include "aeronet/log.hpp"
#include "aeronet/memory-utils-sv.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/static-string-view-helpers.hpp"
#include "aeronet/system-error-message.hpp"
#include "aeronet/system-error.hpp"
#include "aeronet/timedef.hpp"
#include "aeronet/transport-result.hpp"
#include "aeronet/transport.hpp"
#include "http1-response-parser.hpp"
#include "transport-io.hpp"

namespace aeronet::internal {

// Writes the request of an HTTP/1.1 exchange while watching the connection for its response (RFC 9112 §9.5): a final
// response from 300 received before the end of the request, or the closing of the connection, stops the writing, as the
// server will not read the rest. A 2xx response does not: the server may still read the request content. The response
// bytes received meanwhile stay in the receive buffer of the client, where the parsing of the response starts.
class ClientConnection::Http11RequestWriter {
 public:
  Http11RequestWriter(HttpClient& client, Transport& transport, NativeHandle fd, SteadyClock::time_point deadline,
                      bool& requestSent) noexcept
      : _client(client), _transport(transport), _deadline(deadline), _requestSent(requestSent), _fd(fd) {}

  // Write `req`: its head, possibly followed by the start of its body (inlined body or chunked framing), then its
  // captured body, then its file payload. With the 100-continue expectation, what follows the head is only sent once
  // the server answered 100 (Continue), or without answer within HttpClientConfig::expectContinueTimeout. Sets
  // `requestSent` as soon as any byte reaches the transport (the request can no longer be retried). Returns timeout,
  // writeError when nothing could be written or when the file payload could not be read, noError otherwise,
  // interrupted() telling whether the request was not written whole.
  HttpClientErrc write(const HttpRequest& req) {
    const HttpClientConfig& config = _client.config();
    // Trailers require Transfer-Encoding: chunked on the wire (RFC 7230 section 4.1.2). Serialize the whole chunked
    // request on a copy. The original stays protocol-neutral because a retry or redirect may later select HTTP/2, where
    // trailers use a trailing HEADERS frame and Transfer-Encoding is forbidden. The copy only provides the wire data:
    // its headers are queried on the original. Its serialization writes the "close" connection option of a client that
    // does not keep its connections alive (see HttpClient::makeRequestOptions()).
    std::optional<HttpRequest> chunkedReq;
    const HttpRequest& wireReq =
        req.trailersSize() == 0 ? req : chunkedReq.emplace(req.finalizeTrailersForHttp11(config.minCapturedBodySize));
    // The request line, headers and any inline body all live contiguously in the HttpRequest's own buffer
    // (completeRequestForHttp11 is the whole thing minus the internal origin-key prefix). A captured (not inlined) body
    // is streamed separately so it is never copied into the head buffer, and a captured file body is streamed from disk
    // after the head (never fully loaded in memory). The head already carries the exact Content-Length.
    std::string_view wire = wireReq.completeRequestForHttp11();
    // The body of the chunked copy starts after its chunk-size line, which follows the blank line ending its head: the
    // head ends at the first blank line, as no header line holds a CRLF.
    std::size_t headEnd = chunkedReq.has_value() ? wire.find(http::DoubleCRLF) : req.headEndForHttp11();
    assert(wire.substr(headEnd).starts_with(http::DoubleCRLF));
    // RFC 9112 section 9.6: a client that does not keep its connections alive MUST send the "close" connection option.
    // It also lets the server close first, so that the TIME_WAIT state lands on its side instead of piling up on the
    // client ephemeral ports. The serialization of the chunked copy already writes it.
    const bool addClose = !chunkedReq.has_value() && !config.keepAlive && !req.hasHeader(http::Connection);
    const std::string_view cookies = _client.requestCookies();
    if (addClose || !cookies.empty()) {
      headEnd = spliceHeadLines(wire, headEnd, cookies, addClose);
    }
    // A body with trailers is in memory, never a file: when captured, it holds the chunked framing and the trailers.
    const std::string_view body = wireReq.hasBodyCaptured() ? wireReq.capturedPayloadForHttp11() : std::string_view{};
    const bool expectContinue =
        config.expectContinueTimeout != HttpClientConfig::Duration::zero() && req.expectsContinue();
    const Step step =
        writeRequest(wire, headEnd + http::DoubleCRLF.size(), body, wireReq.filePayloadPtr(), expectContinue);
    _interrupted = step == Step::Stop;
    if (step == Step::Timeout) {
      return HttpClientErrc::timeout;
    }
    return step == Step::WriteError ? HttpClientErrc::writeError : HttpClientErrc::noError;
  }

  // Whether the request was not written whole: stopped by a response or by the closing of the connection, or after a
  // failed write.
  [[nodiscard]] bool interrupted() const noexcept { return _interrupted; }

  // Whether the connection was closed (or failed) on the server side while the request was written.
  [[nodiscard]] bool eof() const noexcept { return _eof; }

  // Whether a write failed after part of the request was written: only a response already received can be read.
  [[nodiscard]] bool writeFailed() const noexcept { return _writeFailed; }

 private:
  // Copies 'wire' (the head, then the inline body if any) into the request scratch buffer of the client, idle until the
  // response phase, with 'cookies' added to the Cookie header line (a request has a single one, RFC 6265bis §5.8.3),
  // or in a new one, and with the "close" connection option when 'addClose' is set. The request itself stays
  // protocol-neutral, because a retry or redirect may select HTTP/2, which sends the cookies apart and forbids the
  // Connection header. The captured body, if any, is still written straight from the request. Points 'wire' to the
  // copy and returns its head end.
  std::size_t spliceHeadLines(std::string_view& wire, std::size_t headEnd, std::string_view cookies, bool addClose) {
    static constexpr std::string_view kConnectionCloseLine =
        JoinStringView_v<http::CRLF, http::Connection, http::HeaderSep, http::close>;
    static constexpr std::string_view kCookieLinePrefix = JoinStringView_v<http::CRLF, http::Cookie, http::HeaderSep>;

    // Where the cookies go: at the end of the Cookie line of the request, else in a new line at the end of the head.
    std::size_t cookiesPos = headEnd;
    std::string_view cookiesPrefix = kCookieLinePrefix;
    if (!cookies.empty()) {
      const std::string_view head = wire.substr(0, headEnd);
      if (const std::size_t cookieLinePos = head.find(kCookieLinePrefix); cookieLinePos != std::string_view::npos) {
        const std::size_t valuePos = cookieLinePos + kCookieLinePrefix.size();
        cookiesPos = std::min(head.find(http::CRLF, valuePos), headEnd);
        cookiesPrefix = cookiesPos == valuePos ? std::string_view{} : http::CookiePairSep;
      }
    }
    const std::size_t cookiesSize = cookies.empty() ? 0 : cookiesPrefix.size() + cookies.size();
    const std::size_t closeSize = addClose ? kConnectionCloseLine.size() : 0;

    RawChars& out = _client.bodyBuffer();
    out.reserve(wire.size() + cookiesSize + closeSize);
    char* pData = Append(wire.substr(0, cookiesPos), out.data());
    if (!cookies.empty()) {
      pData = Append(cookiesPrefix, pData);
      pData = Append(cookies, pData);
    }
    pData = Append(wire.substr(cookiesPos, headEnd - cookiesPos), pData);
    if (addClose) {
      pData = AppendFixed<kConnectionCloseLine>(pData);
    }
    out.setEnd(Append(wire.substr(headEnd), pData));
    wire = out;
    return headEnd + cookiesSize + closeSize;
  }

  // What a step of the writing tells about the rest of the request.
  enum class Step : uint8_t {
    Continue,    // the writing goes on
    Stop,        // the request will not be written whole (see interrupted()): its response is read
    Timeout,     // the exchange fails with HttpClientErrc::timeout
    WriteError,  // the exchange fails with HttpClientErrc::writeError
  };

  // Write `wire`, whose first `headSize` bytes are the head, then `body`, then the file payload of `pFilePayload` when
  // not null. With `expectContinue`, what follows the head waits for the answer of the server (see awaitContinue()).
  Step writeRequest(std::string_view wire, std::size_t headSize, std::string_view body, const FilePayload* pFilePayload,
                    bool expectContinue) {
    if (expectContinue) {
      if (const Step step = writeAll(wire.substr(0, headSize), {}); step != Step::Continue) {
        return step;
      }
      if (const Step step = awaitContinue(); step != Step::Continue) {
        return step;
      }
      wire.remove_prefix(headSize);
    }
    if (const Step step = writeAll(wire, body); step != Step::Continue || pFilePayload == nullptr) {
      return step;
    }
    return writeFile(*pFilePayload);
  }

  // Write the head followed by the body, pumping the event loop on would-block. While both buffers are still pending
  // they are sent with a single scatter write (writev / ordered TLS write) so the body is never copied into the head
  // buffer. Resumes correctly across partial writes spanning the two.
  Step writeAll(std::string_view head, std::string_view body) {
    const std::size_t total = head.size() + body.size();
    for (std::size_t off = 0; off < total;) {
      // While head bytes remain, scatter the rest of the head + the whole body in a single writev
      // (PlainTransport) or an ordered head-then-body write (TLS). Once the head is fully flushed,
      // stream the remaining body alone. This avoids copying the body into the head buffer.
      const TransportResult transportRes = off < head.size() ? _transport.write(head.substr(off), body)
                                                             : _transport.write(body.substr(off - head.size()));
      off += transportRes.bytesProcessed;
      if (off != 0) {
        // Some request bytes have reached the transport (the origin may already be processing the request);
        // from here on the exchange must never be retried, so a non-idempotent request is not re-submitted.
        _requestSent = true;
      }
      if (off < total) {
        if (const Step step = resume(transportRes.want); step != Step::Continue) {
          return step;
        }
      }
    }
    return Step::Continue;
  }

  // Stream a captured file body to the transport after the head has been written, reading it from disk in bounded
  // chunks (into HttpClient's reusable scratch buffer) so it is never fully materialized in memory.
  Step writeFile(const FilePayload& filePayload) {
    const File& file = filePayload.file;
    std::size_t fileOffset = filePayload.offset;
    std::size_t remaining = filePayload.length;

    if (_transport.supportsSendfile()) {
      // Plain socket: hand the region to the kernel via sendfile(2). The bytes go straight from the page
      // cache to the socket - never copied into user space (mirrors the server's response sendfile path).
      while (remaining != 0) {
        const TransportResult transportRes = _transport.sendFile(file, fileOffset, remaining);
        if (transportRes.bytesProcessed != 0) {
          _requestSent = true;
          remaining -= transportRes.bytesProcessed;  // sendFile already advanced fileOffset
          continue;
        }
        if (transportRes.want == TransportHint::Error) {
          // Not meaningful when the file ended early (sendfile returning 0), which sets no error.
          const int err = LastSystemError();
          log::error(
              "HTTP/1.1 client: sendfile of request body failed (fd # {}, offset={}, remaining={}, err={}, msg={})",
              _fd, fileOffset, remaining, err, SystemErrorMessage(err));
        }
        if (const Step step = resume(transportRes.want); step != Step::Continue) {
          return step;
        }
      }
      return Step::Continue;
    }

    // TLS (or any transport that cannot sendfile): read the file in bounded chunks and write it through the
    // transport, which encrypts in user space. Stream at most this many bytes per read + write iteration so
    // a large file is never fully materialized in memory (the head already declared the exact Content-Length).
    static constexpr std::size_t kFileChunk = 64UL * 1024UL;
    // Reuse the client's request/body scratch buffer as the file read staging area: it is idle during the
    // send phase (only borrowed later by Http1ResponseParser, which clears it) and reused across requests.
    RawChars& chunkBuf = _client.bodyBuffer();
    chunkBuf.clear();
    while (remaining != 0) {
      const std::size_t toRead = std::min(remaining, kFileChunk);
      chunkBuf.ensureAvailableCapacityExponential(toRead);
      const std::size_t nread =
          file.readAt(std::as_writable_bytes(std::span<char>(chunkBuf.data(), toRead)), fileOffset);
      if (nread == 0 || nread == File::kError) {
        // A short read to 0 (file truncated under us) or an I/O error leaves the declared Content-Length
        // unfulfilled: the request framing is broken, so abort the exchange.
        log::error("HTTP/1.1 client: reading request file body failed (offset={}, remaining={})", fileOffset,
                   remaining);
        return Step::WriteError;
      }
      if (const Step step = writeAll(std::string_view(chunkBuf.data(), nread), {}); step != Step::Continue) {
        return step;
      }
      fileOffset += nread;
      remaining -= nread;
    }
    return Step::Continue;
  }

  // After a write that did not take all the bytes: stops on a transport error, waits for the transport otherwise.
  Step resume(TransportHint want) {
    if (want != TransportHint::Error) {
      return waitWritable(want);
    }
    if (!_requestSent) {
      return Step::WriteError;  // nothing was sent, no response can follow
    }
    // The server may have answered then closed the connection, such as after a 413: its response is read.
    _writeFailed = true;
    return Step::Stop;
  }

  // Wait until the transport can resume the write (`want`), reading the response bytes received meanwhile.
  Step waitWritable(TransportHint want) {
    const EventBmp interest = InterestFor(want);
    if (!_watchResponse) {
      return _client.waitExchangeIo(_fd, interest, _deadline) ? Step::Continue : Step::Timeout;
    }
    const EventBmp events = _client.waitExchangeEvents(_fd, interest | EventIn, _deadline);
    if (events == 0U) {
      return Step::Timeout;
    }
    if ((events & ~EventOut) != 0U) {
      return readEarlyResponse();  // readable, or error / hang-up, that the read tells
    }
    return Step::Continue;
  }

  // The head of a request with the 100-continue expectation is sent: wait for the answer of the server before sending
  // the content (RFC 9110 §10.1.1). It is sent after 100 (Continue), or without answer within expectContinueTimeout,
  // as from a server ignoring expectations. A final response answers the request without it.
  Step awaitContinue() {
    const SteadyClock::time_point continueDeadline =
        std::min(_deadline, SteadyClock::now() + _client.config().expectContinueTimeout);
    while (_early == Http1ResponseParser::EarlyResponse::None) {
      if (_client.waitExchangeEvents(_fd, EventIn, continueDeadline) == 0U) {
        return SteadyClock::now() < _deadline ? Step::Continue : Step::Timeout;
      }
      if (const Step step = readEarlyResponse(); step != Step::Continue) {
        return step;
      }
    }
    return _early == Http1ResponseParser::EarlyResponse::Continue ? Step::Continue : Step::Stop;
  }

  // Read the response bytes received while the request is written, and tell whether they stop it.
  Step readEarlyResponse() {
    const HttpClientConfig& config = _client.config();
    RawChars& received = _client.responseBuffer();
    do {
      const TransportResult transportRes = ReadAppend(_transport, received, config.minReadChunkBytes);
      if (transportRes.bytesProcessed == 0) {
        if (transportRes.want == TransportHint::None || transportRes.want == TransportHint::Error) {
          // Closed (or failed) on the server side: what it answered, if anything, is all there is.
          _eof = true;
          return Step::Stop;
        }
        break;
      }
      // TLS may hold more decrypted bytes than read, which the readiness of the socket does not signal.
    } while (_transport.hasPendingReadData() && received.size() <= config.maxResponseBytes);
    if (received.size() > config.maxResponseBytes) {
      return Step::Stop;  // the response is too large anyway
    }
    _early = Http1ResponseParser::PeekEarlyResponse(received);
    if (_early == Http1ResponseParser::EarlyResponse::Rejection) {
      return Step::Stop;
    }
    if (_early == Http1ResponseParser::EarlyResponse::Success) {
      // The rest of the response is not needed before the end of the request.
      _watchResponse = false;
    }
    return Step::Continue;
  }

  HttpClient& _client;
  Transport& _transport;
  SteadyClock::time_point _deadline;
  bool& _requestSent;
  NativeHandle _fd;
  Http1ResponseParser::EarlyResponse _early{Http1ResponseParser::EarlyResponse::None};
  bool _watchResponse{true};
  bool _interrupted{false};
  bool _eof{false};
  bool _writeFailed{false};
};

HttpClientResult ClientConnection::exchangeForHttp11(HttpClient& client, Transport& transport, NativeHandle fd,
                                                     HttpRequest& req, SteadyClock::time_point ioDeadline,
                                                     bool& requestSent) {
  const HttpClientConfig& config = client.config();
  _keepAlive = false;  // until the response is complete: a failed exchange never leaves the connection reusable
  RawChars& responseBuffer = client.responseBuffer();
  responseBuffer.clear();  // reuse the buffer's allocation across requests (the response may arrive during the write)
  Http11RequestWriter writer(client, transport, fd, ioDeadline, requestSent);
  if (const HttpClientErrc writeErr = writer.write(req); writeErr != HttpClientErrc::noError) {
    return std::unexpected(writeErr);
  }

  // The chunked-body reassembly and raw receive buffers are borrowed from the client. Suitably-sized allocations move
  // into the response and get an equal-capacity replacement; oversized scratch stays with the client while its small
  // body is copied. Both paths preserve reuse without making a later request invalidate the returned response.
  ResponseStreamer& streamer = client._streamer;
  Http1ResponseParser parser(client.bodyBuffer());
  parser.reset(req.method() == http::Method::HEAD, streamer.active());
  if (config.decompression.enable) {
    // Decode Content-Encoding'd response bodies in place at install time (straight from the receive
    // buffer / de-framed chunk buffer, no intermediate copy of the compressed bytes).
    parser.setDecodeContext({
        .pState = &client._codec.decompressionState,
        .pConfig = &config.decompression,
        .pOut = &client._codec.decompressOut,
        .pTmp = &client._codec.decompressTmp,
    });
  }
  bool eof = writer.eof();
  const auto minReadChunkBytes = config.minReadChunkBytes;

  HttpResponse resp;
  // The response bytes received while the request was written are parsed before any read.
  for (bool read = responseBuffer.empty();; read = true) {
    if (read) {
      const TransportResult transportRes = ReadAppend(transport, responseBuffer, minReadChunkBytes);
      if (transportRes.bytesProcessed == 0) {
        if (transportRes.want == TransportHint::ReadReady || transportRes.want == TransportHint::WriteReady) {
          if (writer.writeFailed()) {
            // No complete response came before the failure of the write, nor will on a broken connection.
            return std::unexpected(HttpClientErrc::writeError);
          }
          if (!client.waitExchangeIo(fd, InterestFor(transportRes.want), ioDeadline)) {
            return std::unexpected(HttpClientErrc::timeout);
          }
          continue;
        }
        // 0 bytes, no want => orderly close.
        eof = true;
      }
    }

    Http1ResponseParser::Status st = parser.parse(responseBuffer, eof, resp, config.maxResponseBytes);
    if (st == Http1ResponseParser::Status::HeadComplete) {
      // Final response head of a sink request: deliver its body as it arrives, unless it is buffered (a redirect to
      // follow, a status to retry, stacked content codings).
      bool stream;
      if (const HttpClientErrc err = streamer.startResponse(
              resp, {.contentType = parser.contentType(responseBuffer), .size = parser.announcedBodySize()}, stream);
          err != HttpClientErrc::noError) {
        return std::unexpected(err);
      }
      if (stream) {
        parser.startStreaming();
      }
      st = parser.parse(responseBuffer, eof, resp, config.maxResponseBytes);
    }
    // A streamed body comes in pieces, each delivered as soon as it is parsed (also before a failure that follows it).
    while (st == Http1ResponseParser::Status::BodyPiece) {
      if (!streamer.feed(parser.streamedBody(responseBuffer))) {
        return std::unexpected(streamer.error());
      }
      st = parser.parse(responseBuffer, eof, resp, config.maxResponseBytes);
    }
    if (parser.streaming()) {
      parser.discardStreamed(responseBuffer);  // so that the receive buffer never grows beyond a read
    }
    switch (st) {
      case Http1ResponseParser::Status::NeedMore:
        // Guaranteed by the parser: once eof is set, it always resolves to a final status.
        assert(!eof);
        continue;
      case Http1ResponseParser::Status::Complete:
        break;
      case Http1ResponseParser::Status::Truncated:
        if (writer.writeFailed()) {
          return std::unexpected(HttpClientErrc::writeError);
        }
        log::error("HTTP/1.1 client: {} closed the connection before the end of the response", req.originKey());
        return std::unexpected(HttpClientErrc::connectionClosed);
      case Http1ResponseParser::Status::TooLarge:
        log::error("HTTP/1.1 client: response of {} exceeds maxResponseBytes ({}) or the decompression limits",
                   req.originKey(), config.maxResponseBytes);
        return std::unexpected(HttpClientErrc::responseTooLarge);
      default:
        // HeadComplete is only returned once per response, and BodyPiece until the pieces are consumed: both above.
        assert(st == Http1ResponseParser::Status::Error);
        log::error("HTTP/1.1 client: malformed response from {}", req.originKey());
        return std::unexpected(HttpClientErrc::malformedResponse);
    }
    if (parser.streaming()) {
      if (!streamer.finish()) {
        return std::unexpected(streamer.error());
      }
    } else if (streamer.active()) {
      // Received whole: the body stays in the scratch buffers, delivered by HttpClient if it is the final response.
      streamer.setBufferedBody(parser.bufferedBody(), parser.contentType(responseBuffer));
    }
    break;
  }

  // Bytes still held by the transport (decrypted by TLS but not read yet) were received after the end of the response,
  // like the ones following it in the receive buffer (see Http1ResponseParser::keepAlive()). The ones still in the
  // socket are found by the probe of the idle connection before its reuse. A request not written whole leaves the
  // server expecting its end.
  _keepAlive = parser.keepAlive() && !transport.hasPendingReadData() && !writer.interrupted();
  return resp;
}

}  // namespace aeronet::internal
