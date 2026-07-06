#include <cassert>
#include <charconv>
#include <cstddef>
#include <expected>
#include <string_view>

#include "aeronet/client-connection.hpp"
#include "aeronet/client-request.hpp"
#include "aeronet/encoding.hpp"
#include "aeronet/event.hpp"
#include "aeronet/http-client-config.hpp"
#include "aeronet/http-client-error.hpp"
#include "aeronet/http-client.hpp"
#include "aeronet/http-codec.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-message.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/memory-utils-sv.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/ndigits.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/string-equal-ignore-case.hpp"
#include "aeronet/timedef.hpp"
#include "aeronet/transport.hpp"
#include "aeronet/url.hpp"
#include "client-request-builder.hpp"
#include "http-client-codec.hpp"
#include "response-parser.hpp"

namespace aeronet::internal {

namespace {

constexpr bool MethodUsuallyHasBody(http::Method method) noexcept {
  return method == http::Method::POST || method == http::Method::PUT || method == http::Method::PATCH;
}

// Request-line and framing literals emitted into the head buffer by buildRequestBytes. Header-name
// prefixes are kept in their canonical capitalized wire form here (http-constants.hpp stores lowercase
// names for the server's case-insensitive matching) and reused so each is reserved / copied once.
constexpr std::string_view kRequestLineSuffix = " HTTP/1.1\r\n";  // follows "<method> <target>"
constexpr std::string_view kHostHeaderPrefix = "Host: ";
constexpr std::string_view kUserAgentHeaderPrefix = "User-Agent: ";
constexpr std::string_view kAcceptEncodingHeaderPrefix = "Accept-Encoding: ";
constexpr std::string_view kConnectionCloseLine = "Connection: close\r\n";
constexpr std::string_view kContentEncodingHeaderPrefix = "Content-Encoding: ";
constexpr std::string_view kContentLengthHeaderPrefix = "Content-Length: ";
constexpr std::string_view kContentLengthZeroLine = "Content-Length: 0\r\n";

}  // namespace

std::string_view ClientConnection::maybeCompressRequestBody(HttpClient& client, std::string_view body,
                                                            bool hasContentEncoding, bool hasTransferEncoding,
                                                            std::string_view& contentEncoding) {
  const HttpClientConfig::RequestCompression& rc = client.config().requestCompression;
  if (rc.enabled() && !body.empty() && body.size() >= rc.codec.minBytes && IsEncodingEnabled(rc.encoding) &&
      !hasContentEncoding && !hasTransferEncoding) {
    const std::size_t maxCompressedBytes = rc.codec.maxCompressedBytes(body.size());
    // client.codec() is materialized lazily here: only a request that actually compresses pays for the codec.
    const std::string_view compressed = HttpCodec::CompressFullBody(client.codec().compressionState, rc.encoding, body,
                                                                    maxCompressedBytes, client.codec().compressOut);
    if (!compressed.empty()) {
      contentEncoding = GetEncodingStr(rc.encoding);
      return compressed;
    }
  }
  return body;
}

std::string_view ClientConnection::buildRequestBytesForHttp11(HttpClient& client, const Url& url,
                                                              const ClientRequest& req, http::Method method,
                                                              bool dropBody) {
  const HttpClientConfig& config = client.config();
  RawChars& requestBuffer = client.requestBuffer();
  requestBuffer.clear();

  // HttpMessage (reused as the field container) manages Content-Type / Content-Length and stores
  // user headers; they all appear in headersFlatView(). We only inject the request-specific framing
  // headers (Host / User-Agent / Accept-Encoding / Connection) when the user did not set them.
  const std::string_view hostStr = url.host();
  // The host is stored unbracketed even for IPv6 literals (a parsed host only ever contains ':' when it is
  // an IPv6 address), so the Host header must re-add the brackets: "Host: [::1]:8080", never "::1:8080".
  const bool hostIsIpv6 = hostStr.contains(':');

  // Request line: "<method> <target> HTTP/1.1\r\n" -- one capacity check for the whole line.
  const std::string_view methodStr = http::MethodToStr(method);
  const std::string_view target = url.target();
  const std::string_view userAgent = config.userAgent();

  // Single scan of the user headers, then derive which framing headers we still inject (see the shared
  // request-builder helpers -- HTTP/2 mirrors these same decisions).
  const RequestHeaderScan scan = ScanRequestHeaders(req.headers());
  const std::string_view contentType = scan.contentType;
  const bool addHostHeader = scan.host.empty();
  const bool addUserAgent = !userAgent.empty() && !scan.hasUserAgent;
  const bool addConnectionClose = !config.keepAlive && !scan.hasConnection;
  const std::string_view acceptEncoding = ResolveAcceptEncoding(config, scan.hasAcceptEncoding);
  const bool addAcceptEncoding = !acceptEncoding.empty();

  // Optional outbound compression of a large request body (opt-in). On success `contentEncoding` becomes
  // non-empty and `body` points at the compressed bytes held in the codec's reusable buffer.
  std::string_view contentEncoding;
  std::string_view body = maybeCompressRequestBody(client, dropBody ? std::string_view{} : req.body(),
                                                   scan.hasContentEncoding, scan.hasTransferEncoding, contentEncoding);

  std::size_t neededSize = methodStr.size() + 1U + target.size() + kRequestLineSuffix.size() + http::CRLF.size();

  if (addHostHeader) {
    neededSize += kHostHeaderPrefix.size() + AuthorityLen(url, hostIsIpv6) + http::CRLF.size();
  }
  if (addUserAgent) {
    neededSize += kUserAgentHeaderPrefix.size() + userAgent.size() + http::CRLF.size();
  }
  if (addAcceptEncoding) {
    neededSize += kAcceptEncodingHeaderPrefix.size() + acceptEncoding.size() + http::CRLF.size();
  }
  if (addConnectionClose) {
    neededSize += kConnectionCloseLine.size();
  }

  const auto bodySz = body.size();
  const auto ndigitsBodySz = ndigits(bodySz);
  const bool addZeroContentLength =
      body.empty() && MethodUsuallyHasBody(method) && !scan.hasContentLength && !scan.hasTransferEncoding;
  const bool concatenateSmallBody = !body.empty() && bodySz <= config.maxCapturedRequestBodyBytes;
  const std::string_view headersFlatView = req.headersFlatView();

  neededSize += headersFlatView.size();
  if (dropBody) {
    neededSize -= HttpMessage::HeaderSize(http::ContentLength.size(), ndigitsBodySz) +
                  HttpMessage::HeaderSize(http::ContentType.size(), contentType.size());
  } else if (!contentEncoding.empty()) {
    neededSize -= HttpMessage::HeaderSize(http::ContentLength.size(), ndigitsBodySz);
    neededSize += kContentEncodingHeaderPrefix.size() + contentEncoding.size() + http::CRLF.size() +
                  kContentLengthHeaderPrefix.size() + ndigitsBodySz + http::CRLF.size();
  } else if (addZeroContentLength) {
    neededSize += kContentLengthZeroLine.size();
  }
  if (concatenateSmallBody) {
    neededSize += bodySz;
  }

  requestBuffer.ensureAvailableCapacity(neededSize);

  char* pEnd = requestBuffer.data() + requestBuffer.size();

  pEnd = Append(methodStr, pEnd);
  *pEnd++ = ' ';
  pEnd = Append(target, pEnd);
  pEnd = Append(kRequestLineSuffix, pEnd);

  // Append one "name: value\r\n" header line under a single capacity check.
  const auto appendHeaderLine = [&pEnd](std::string_view name, std::string_view value) {
    pEnd = Append(name, pEnd);
    pEnd = Append(http::HeaderSep, pEnd);
    pEnd = Append(value, pEnd);
    pEnd = Append(http::CRLF, pEnd);
  };

  if (addHostHeader) {
    // "Host: " + authority ("[host]:port", brackets for IPv6 literals, port written straight into the
    // buffer tail) + CRLF.
    pEnd = Append(kHostHeaderPrefix, pEnd);
    pEnd = AppendAuthority(pEnd, url, hostIsIpv6);
    pEnd = Append(http::CRLF, pEnd);
  }
  if (addUserAgent) {
    pEnd = Append(kUserAgentHeaderPrefix, pEnd);
    pEnd = Append(userAgent, pEnd);
    pEnd = Append(http::CRLF, pEnd);
  }
  // Accept-Encoding: an explicit defaultAcceptEncoding wins; otherwise advertise what we can decode when
  // response decompression is enabled (so origins know they may compress and we will transparently decode).
  if (addAcceptEncoding) {
    pEnd = Append(kAcceptEncodingHeaderPrefix, pEnd);
    pEnd = Append(acceptEncoding, pEnd);
    pEnd = Append(http::CRLF, pEnd);
  }
  if (addConnectionClose) {
    pEnd = Append(kConnectionCloseLine, pEnd);
  }

  if (dropBody) {
    // Redirect rewrite to GET: emit user headers but drop the body's Content-Type / Content-Length.
    for (const auto& [name, value] : req.headers()) {
      if (CaseInsensitiveEqual(name, http::ContentType) || CaseInsensitiveEqual(name, http::ContentLength)) {
        continue;
      }
      appendHeaderLine(name, value);
    }
  } else if (!contentEncoding.empty()) {
    // Compressed body: emit the managed/user headers but rewrite Content-Length to the compressed size and
    // append the Content-Encoding header.
    for (const auto& [name, value] : req.headers()) {
      if (CaseInsensitiveEqual(name, http::ContentLength)) {
        continue;
      }
      appendHeaderLine(name, value);
    }
    // "Content-Encoding: <enc>\r\nContent-Length: <len>\r\n" -- one reservation, length written in place.
    pEnd = Append(kContentEncodingHeaderPrefix, pEnd);
    pEnd = Append(contentEncoding, pEnd);
    pEnd = Append(http::CRLF, pEnd);
    pEnd = Append(kContentLengthHeaderPrefix, pEnd);

    pEnd = std::to_chars(pEnd, pEnd + ndigitsBodySz, bodySz).ptr;
    pEnd = Append(http::CRLF, pEnd);
  } else {
    pEnd = Append(headersFlatView, pEnd);
    if (addZeroContentLength) {
      pEnd = Append(kContentLengthZeroLine, pEnd);
    }
  }

  // End-of-header CRLF. Small bodies are folded into the head buffer (same reservation) for a single
  // contiguous write (one syscall / one TLS record) instead of a scatter writev; larger bodies stay
  // zero-copy and are streamed alongside the head.
  pEnd = Append(http::CRLF, pEnd);  // end of header block
  if (concatenateSmallBody) {
    pEnd = Append(body, pEnd);
    body = {};
  }
  requestBuffer.setSize(static_cast<std::size_t>(pEnd - requestBuffer.data()));
  return body;
}

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
                                                     const Url& url, const ClientRequest& req, http::Method method,
                                                     bool dropBody, SteadyClock::time_point ioDeadline,
                                                     bool& requestSent) {
  const HttpClientConfig& config = client.config();
  const std::string_view body = buildRequestBytesForHttp11(client, url, req, method, dropBody);
  if (auto wr = writeAllForHttp11(client, transport, fd, client.requestBuffer(), body, ioDeadline, requestSent); !wr) {
    return std::unexpected(wr.error());
  }

  // The chunked-body reassembly buffer is borrowed from the client (reused across exchanges) rather than
  // owned by the parser, so a keep-alive connection streaming chunked responses never re-grows it.
  ResponseParser parser(client.bodyBuffer());
  parser.reset(method == http::Method::HEAD);
  if (config.decompression.enable) {
    // Decode Content-Encoding'd response bodies in place at install time (straight from the receive
    // buffer / de-framed chunk buffer, no intermediate copy of the compressed bytes).
    parser.setDecodeContext({.state = &client.codec().decompressionState,
                             .config = &config.decompression,
                             .out = &client.codec().decompressOut,
                             .tmp = &client.codec().decompressTmp});
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
