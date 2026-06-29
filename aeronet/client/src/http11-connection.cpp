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

// Comma-separated list of the content codings this build can decode, in aeronet's preference order.
// Advertised as the default Accept-Encoding when response decompression is enabled and the user did not
// set an explicit value. Built once at compile time from the codecs compiled in.
namespace details {
struct AcceptEncodingCoding {
  std::string_view name;
  bool enabled;
};

// Preference order; gzip and deflate both ride on zlib.
inline constexpr AcceptEncodingCoding kAcceptEncodingCodings[]{
    {http::zstd, IsEncodingEnabled(Encoding::zstd)},
    {http::br, IsEncodingEnabled(Encoding::br)},
    {http::gzip, IsEncodingEnabled(Encoding::gzip)},
    {http::deflate, IsEncodingEnabled(Encoding::deflate)},
};

// Exact byte length of the comma-separated list of the enabled codings (separators included).
constexpr std::size_t ComputeAcceptEncodingSize() {
  std::size_t size = 0;
  for (const auto& coding : kAcceptEncodingCodings) {
    if (coding.enabled) {
      if (size != 0) {
        size += 2;  // ", " separator
      }
      size += coding.name.size();
    }
  }
  return size;
}

// Static storage holding exactly the list bytes; the +1 keeps the array non-empty and null-terminated.
struct AcceptEncodingStorage {
  char storage[ComputeAcceptEncodingSize() + 1]{};
};

constexpr AcceptEncodingStorage MakeAcceptEncoding() {
  AcceptEncodingStorage out;
  std::size_t pos = 0;
  for (const auto& coding : kAcceptEncodingCodings) {
    if (coding.enabled) {
      if (pos != 0) {
        out.storage[pos++] = ',';
        out.storage[pos++] = ' ';
      }
      for (char ch : coding.name) {
        out.storage[pos++] = ch;
      }
    }
  }
  return out;
}

inline constexpr AcceptEncodingStorage kAcceptEncodingStorage = MakeAcceptEncoding();
}  // namespace details

inline constexpr std::string_view kSupportedAcceptEncoding{details::kAcceptEncodingStorage.storage,
                                                           details::ComputeAcceptEncodingSize()};

}  // namespace

std::string_view ClientConnection::buildRequestBytesForHttp11(ClientHost& host, const Url& url,
                                                              const ClientRequest& req, http::Method method,
                                                              bool dropBody) {
  const HttpClientConfig& config = host.config();
  RawChars& requestBuffer = host.requestBuffer();
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
  std::string_view contentType;

  bool addHostHeader = true;
  bool addUserAgent = !userAgent.empty();
  bool addAcceptEncoding = true;
  bool addConnectionClose = !config.keepAlive;
  bool hasTransferEncoding = false;
  bool hasContentEncoding = false;
  bool hasContentLength = false;
  for (const auto& [name, value] : req.headers()) {
    if (CaseInsensitiveEqual(name, http::Host)) {
      addHostHeader = false;
    } else if (CaseInsensitiveEqual(name, http::UserAgent)) {
      addUserAgent = false;
    } else if (CaseInsensitiveEqual(name, http::AcceptEncoding)) {
      addAcceptEncoding = false;
    } else if (CaseInsensitiveEqual(name, http::Connection)) {
      addConnectionClose = false;
    } else if (CaseInsensitiveEqual(name, http::TransferEncoding)) {
      hasTransferEncoding = true;
    } else if (CaseInsensitiveEqual(name, http::ContentEncoding)) {
      hasContentEncoding = true;
    } else if (CaseInsensitiveEqual(name, http::ContentLength)) {
      hasContentLength = true;
    } else if (CaseInsensitiveEqual(name, http::ContentType)) {
      contentType = value;
    }
  }

  const auto ndigitsPort = ndigits(url.port());

  std::string_view body = dropBody ? std::string_view{} : req.body();

  // Optional outbound compression of a large request body (opt-in). On success `contentEncoding` becomes
  // non-empty and `body` points at the compressed bytes held in the codec's reusable buffer.
  std::string_view contentEncoding;
  if (!dropBody && config.requestCompression.enabled() && !body.empty()) {
    const HttpClientConfig::RequestCompression& rc = config.requestCompression;
    if (body.size() >= rc.codec.minBytes && IsEncodingEnabled(rc.encoding) && !hasContentEncoding &&
        !hasTransferEncoding) {
      const std::size_t maxCompressedBytes = rc.codec.maxCompressedBytes(body.size());
      const std::string_view compressed = internal::HttpCodec::CompressFullBody(
          host.codec().compressionState, rc.encoding, body, maxCompressedBytes, host.codec().compressOut);
      if (!compressed.empty()) {
        body = compressed;
        contentEncoding = GetEncodingStr(rc.encoding);
      }
    }
  }

  std::size_t neededSize = methodStr.size() + 1U + target.size() + kRequestLineSuffix.size() + http::CRLF.size();

  if (addHostHeader) {
    neededSize += kHostHeaderPrefix.size() + hostStr.size() + http::CRLF.size();
    if (hostIsIpv6) {
      neededSize += 2U;  // surrounding '[' and ']'
    }
    if (!url.isDefaultPort()) {
      neededSize += 1U + ndigitsPort;
    }
  }
  if (addUserAgent) {
    neededSize += kUserAgentHeaderPrefix.size() + userAgent.size() + http::CRLF.size();
  }
  std::string_view acceptEncoding = config.defaultAcceptEncoding();
  if (addAcceptEncoding) {
    if (acceptEncoding.empty() && config.decompression.enable) {
      acceptEncoding = kSupportedAcceptEncoding;
    }
    if (acceptEncoding.empty()) {
      addAcceptEncoding = false;
    } else {
      neededSize += kAcceptEncodingHeaderPrefix.size() + acceptEncoding.size() + http::CRLF.size();
    }
  }
  if (addConnectionClose) {
    neededSize += kConnectionCloseLine.size();
  }

  const auto bodySz = body.size();
  const auto ndigitsBodySz = ndigits(bodySz);
  const bool addZeroContentLength =
      body.empty() && MethodUsuallyHasBody(method) && !hasContentLength && !hasTransferEncoding;
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
    // "Host: " + host (bracketed for IPv6 literals) + optional ":<port>" + CRLF; the port is written
    // straight into the buffer tail.
    pEnd = Append(kHostHeaderPrefix, pEnd);
    if (hostIsIpv6) {
      *pEnd++ = '[';
    }
    pEnd = Append(hostStr, pEnd);
    if (hostIsIpv6) {
      *pEnd++ = ']';
    }
    if (!url.isDefaultPort()) {
      *pEnd++ = ':';
      pEnd = std::to_chars(pEnd, pEnd + ndigitsPort, url.port()).ptr;
    }
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

std::expected<void, HttpClientErrc> ClientConnection::writeAllForHttp11(ClientHost& host, ITransport& transport,
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
    if (!host.waitIo(fd, interest, deadline)) {
      return std::unexpected(HttpClientErrc::timeout);
    }
  }
  return {};
}

HttpClientResult ClientConnection::exchangeForHttp11(ClientHost& host, ITransport& transport, NativeHandle fd,
                                                     const Url& url, const ClientRequest& req, http::Method method,
                                                     bool dropBody, SteadyClock::time_point ioDeadline,
                                                     bool& requestSent) {
  const HttpClientConfig& config = host.config();
  const std::string_view body = buildRequestBytesForHttp11(host, url, req, method, dropBody);
  if (auto wr = writeAllForHttp11(host, transport, fd, host.requestBuffer(), body, ioDeadline, requestSent); !wr) {
    return std::unexpected(wr.error());
  }

  ResponseParser parser;
  parser.reset(method == http::Method::HEAD);
  if (config.decompression.enable) {
    // Decode Content-Encoding'd response bodies in place at install time (straight from the receive
    // buffer / de-framed chunk buffer, no intermediate copy of the compressed bytes).
    parser.setDecodeContext({.state = &host.codec().decompressionState,
                             .config = &config.decompression,
                             .out = &host.codec().decompressOut,
                             .tmp = &host.codec().decompressTmp});
  }
  HttpResponse resp;
  RawChars& responseBuffer = host.responseBuffer();
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
    // NeedMore: read straight into the buffer's tail (no bounce buffer, no extra copy).
    if (eof) {
      return std::unexpected(HttpClientErrc::connectionClosed);
    }
    responseBuffer.ensureAvailableCapacityExponential(kReadChunk);
    const ITransport::TransportResult transportRes =
        transport.read(responseBuffer.data() + responseBuffer.size(), kReadChunk);
    if (transportRes.bytesProcessed > 0) {
      responseBuffer.addSize(transportRes.bytesProcessed);
      continue;
    }
    if (transportRes.want == TransportHint::ReadReady) {
      if (!host.waitIo(fd, EventIn, ioDeadline)) {
        return std::unexpected(HttpClientErrc::timeout);
      }
      continue;
    }
    if (transportRes.want == TransportHint::WriteReady) {
      if (!host.waitIo(fd, EventOut, ioDeadline)) {
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
