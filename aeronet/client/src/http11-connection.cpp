#include <cassert>
#include <cstddef>
#include <expected>
#include <string_view>

#include "aeronet/client-connection.hpp"
#include "aeronet/event.hpp"
#include "aeronet/http-client-codec.hpp"
#include "aeronet/http-client-config.hpp"
#include "aeronet/http-client-error.hpp"
#include "aeronet/http-client.hpp"
#include "aeronet/http-message.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/timedef.hpp"
#include "aeronet/transport.hpp"
#include "response-parser.hpp"

namespace aeronet::internal {

std::expected<void, HttpClientErrc> ClientConnection::writeAllForHttp11(HttpClient& client, ITransport& transport,
                                                                        NativeHandle fd, std::string_view head,
                                                                        std::string_view body,
                                                                        SteadyClock::time_point deadline,
                                                                        bool& requestSent) {
  const std::size_t total = head.size() + body.size();
  std::size_t off = 0;
  while (off < total) {
    // While head bytes remain, scatter the rest of the head + the whole body in a single writev
    // (PlainTransport) or an ordered head-then-body write (TLS). Once the head is fully flushed,
    // stream the remaining body alone. This avoids copying the body into the head buffer.
    const ITransport::TransportResult transportRes =
        off < head.size() ? transport.write(head.substr(off), body) : transport.write(body.substr(off - head.size()));
    off += transportRes.bytesProcessed;
    if (off != 0) {
      // Some request bytes have reached the transport (the origin may already be processing the request);
      // from here on the exchange must never be retried, so a non-idempotent request is not re-submitted.
      requestSent = true;
    }
    if (off >= total) {
      break;
    }
    if (transportRes.want == TransportHint::Error) {
      return std::unexpected(HttpClientErrc::writeError);
    }
    const EventBmp interest = (transportRes.want == TransportHint::ReadReady) ? EventIn : EventOut;
    if (!client.waitIo(fd, interest, deadline)) {
      return std::unexpected(HttpClientErrc::timeout);
    }
  }
  return {};
}

HttpClientResult ClientConnection::exchangeForHttp11(HttpClient& client, ITransport& transport, NativeHandle fd,
                                                     HttpRequest& req, SteadyClock::time_point ioDeadline,
                                                     bool& requestSent) {
  const HttpClientConfig& config = client.config();
  // The request line, headers and any inline body all live contiguously in the HttpRequest's own buffer
  // (completeRequestForHttp11 is the whole thing minus the internal origin-key prefix). A captured (not
  // inlined) body is streamed separately so it is never copied into the head buffer. Note: we must NOT
  // reuse client.requestBuffer() here as the head -- for a tunnelled https proxy request it still holds the
  // CONNECT line written by establishProxyTunnel.
  if (req.trailersSize() != 0) {
    // Trailers require Transfer-Encoding: chunked on the wire (RFC 7230 section 4.1.2). Serialize the whole
    // chunked request (with trailers).
    // TODO: is it possible for the same request to be later sent with HTTP/2?
    req.finalizeTrailersForHttp11(config.minCapturedBodySize);
  }
  std::string_view head = req.completeRequestForHttp11();
  std::string_view body = req.hasBodyCaptured() ? req.bodyInMemory() : std::string_view{};
  if (auto wr = writeAllForHttp11(client, transport, fd, head, body, ioDeadline, requestSent); !wr) {
    return std::unexpected(wr.error());
  }

  // The chunked-body reassembly buffer is borrowed from the client (reused across exchanges) rather than
  // owned by the parser, so a keep-alive connection streaming chunked responses never re-grows it.
  ResponseParser parser(client.bodyBuffer());
  parser.reset(req.method() == http::Method::HEAD);
  if (config.decompression.enable) {
    // Decode Content-Encoding'd response bodies in place at install time (straight from the receive
    // buffer / de-framed chunk buffer, no intermediate copy of the compressed bytes).
    parser.setDecodeContext({.state = &client._codec.decompressionState,
                             .config = &config.decompression,
                             .out = &client._codec.decompressOut,
                             .tmp = &client._codec.decompressTmp});
  }
  HttpResponse resp;
  RawChars& responseBuffer = client.responseBuffer();
  responseBuffer.clear();  // reuse the buffer's allocation across requests
  bool eof = false;
  static constexpr std::size_t kReadChunk = 16384;

  for (;;) {
    const ResponseParser::Status st = parser.parse(responseBuffer, eof, resp, config.maxResponseBytes);
    if (st == ResponseParser::Status::Complete) {
      break;
    }
    if (st == ResponseParser::Status::Error) {
      return std::unexpected(HttpClientErrc::malformedResponse);
    }
    // NeedMore: read straight into the buffer's tail (no bounce buffer, no extra copy). The parser resolves
    // every framing to Complete/Error once eof is set (each NeedMore return is guarded by !eof), so reaching
    // NeedMore here always means the connection is still open and more bytes can arrive.
    assert(!eof);
    responseBuffer.ensureAvailableCapacityExponential(kReadChunk);
    const ITransport::TransportResult transportRes =
        transport.read(responseBuffer.data() + responseBuffer.size(), kReadChunk);
    if (transportRes.bytesProcessed > 0) {
      responseBuffer.addSize(transportRes.bytesProcessed);
      continue;
    }
    if (transportRes.want == TransportHint::ReadReady) {
      if (!client.waitIo(fd, EventIn, ioDeadline)) {
        return std::unexpected(HttpClientErrc::timeout);
      }
      continue;
    }
    if (transportRes.want == TransportHint::WriteReady) {
      if (!client.waitIo(fd, EventOut, ioDeadline)) {
        return std::unexpected(HttpClientErrc::timeout);
      }
      continue;
    }
    // 0 bytes, no want => orderly close.
    eof = true;
  }

  _keepAlive = parser.keepAlive();
  return resp;
}

}  // namespace aeronet::internal
