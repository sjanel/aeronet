#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <utility>

#include "accept-encoding-negotiation.hpp"
#include "aeronet/compression-config.hpp"
#include "aeronet/encoding.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server.hpp"
#include "aeronet/http-version.hpp"
#include "connection-state.hpp"
#include "connection.hpp"
#include "log.hpp"
#include "raw-chars.hpp"
#include "string-equal-ignore-case.hpp"
#include "timedef.hpp"
#include "transport.hpp"

namespace aeronet {
void HttpServer::finalizeAndSendResponse(ConnectionMapIt cnxIt, HttpRequest& req, HttpResponse& resp,
                                         std::size_t consumedBytes, std::chrono::steady_clock::time_point reqStart) {
  ConnectionState& state = cnxIt->second;
  ++state.requestsServed;
  bool keepAlive = _config.enableKeepAlive && state.requestsServed < _config.maxRequestsPerConnection;
  if (keepAlive) {
    std::string_view connVal = req.headerValueOrEmpty(http::Connection);
    if (connVal.empty()) {
      // Default is keep-alive for HTTP/1.1, close for HTTP/1.0
      keepAlive = req.version() == http::HTTP_1_1;
    } else {
      if (CaseInsensitiveEqual(connVal, http::close)) {
        keepAlive = false;
      } else if (CaseInsensitiveEqual(connVal, http::keepalive)) {
        keepAlive = true;
      }
    }
  }

  bool isHead = (req.method() == http::Method::HEAD);
  if (!isHead && !resp.userProvidedContentEncoding()) {
    const CompressionConfig& compressionConfig = _config.compression;
    std::string_view encHeader = req.headerValueOrEmpty(http::AcceptEncoding);
    auto [encoding, reject] = _encodingSelector.negotiateAcceptEncoding(encHeader);
    // If the client explicitly forbids identity (identity;q=0) and we have no acceptable
    // alternative encodings to offer, emit a 406 per RFC 9110 Section 12.5.3 guidance.
    if (reject) {
      resp.statusCode(406)
          .reason(http::ReasonNotAcceptable)
          .contentType(http::ContentTypeTextPlain)
          .body("No acceptable content-coding available");
    }
    // Apply size threshold for non-streaming (buffered) responses: if body below minBytes skip compression.
    else if (encoding != Encoding::none && resp.body().size() < compressionConfig.minBytes) {
      encoding = Encoding::none;
    }
    // Approximate allowlist check
    if (encoding != Encoding::none && !compressionConfig.contentTypeAllowlist.empty()) {
      std::string_view contentType = req.headerValueOrEmpty(http::ContentType);
      if (std::ranges::none_of(compressionConfig.contentTypeAllowlist,
                               [contentType](std::string_view str) { return contentType.starts_with(str); })) {
        encoding = Encoding::none;
      }
    }
    if (encoding != Encoding::none) {
      auto& encoder = _encoders[static_cast<size_t>(encoding)];
      if (encoder) {
        auto out = encoder->encodeFull(compressionConfig.encoderChunkSize, resp.body());
        resp.customHeader(http::ContentEncoding, GetEncodingStr(encoding));
        if (compressionConfig.addVaryHeader) {
          resp.customHeader(http::Vary, http::AcceptEncoding);
        }
        resp.body(out);
      }
    }
  }

  queueData(cnxIt, resp.finalizeAndStealData(req.version(), Clock::now(), keepAlive, _config.globalHeaders, isHead,
                                             _config.minCapturedBodySize));

  state.buffer.erase_front(consumedBytes);
  if (!keepAlive && state.outBuffer.empty()) {
    state.requestDrainAndClose();
  }
  if (_metricsCb) {
    RequestMetrics metrics;
    metrics.method = req.method();
    metrics.path = req.path();
    metrics.status = resp.statusCode();
    metrics.bytesIn = req.body().size();
    metrics.reusedConnection = state.requestsServed > 0;
    metrics.duration = std::chrono::steady_clock::now() - reqStart;
    _metricsCb(metrics);
  }
}

bool HttpServer::queueData(ConnectionMapIt cnxIt, HttpResponseData httpResponseData) {
  ConnectionState& state = cnxIt->second;

  const auto totalSz = httpResponseData.remainingSize();

  if (state.outBuffer.empty()) {
    // Plain TCP path: try immediate write optimization
    Transport want;
    const std::size_t written = state.transportWrite(httpResponseData, want);
    switch (want) {
      case Transport::Error:
        state.requestImmediateClose();
        return false;
      case Transport::ReadReady:
        [[fallthrough]];
      case Transport::WriteReady:
        [[fallthrough]];
      case Transport::None:
        if (std::cmp_equal(written, totalSz)) {
          _stats.totalBytesQueued += static_cast<uint64_t>(totalSz);
          _stats.totalBytesWrittenImmediate += static_cast<uint64_t>(written);
          return true;
        }
        // partial write, capture the buffer in the connection state
        httpResponseData.addOffset(static_cast<std::size_t>(written));
        state.outBuffer = std::move(httpResponseData);
        _stats.totalBytesWrittenImmediate += static_cast<uint64_t>(written);
        break;
    }
  } else {
    state.outBuffer.append(std::move(httpResponseData));
  }

  const std::size_t remainingSize = state.outBuffer.remainingSize();
  _stats.totalBytesQueued += static_cast<uint64_t>(totalSz);
  _stats.maxConnectionOutboundBuffer = std::max(_stats.maxConnectionOutboundBuffer, remainingSize);
  if (remainingSize > _config.maxOutboundBufferBytes) {
    state.requestImmediateClose();
  }
  if (!state.waitingWritable) {
    if (HttpServer::ModWithCloseOnFailure(_eventLoop, cnxIt, EPOLLIN | EPOLLOUT | EPOLLET,
                                          "enable writable buffered path", _stats)) {
      state.waitingWritable = true;
      ++_stats.deferredWriteEvents;
    }
  }

  // If we buffered data, try flushing it immediately
  if (!state.outBuffer.empty()) {
    flushOutbound(cnxIt);
  }

  return true;
}

void HttpServer::flushOutbound(ConnectionMapIt cnxIt) {
  ++_stats.flushCycles;
  Transport want = Transport::None;
  ConnectionState& state = cnxIt->second;
  const int fd = cnxIt->first.fd();
  while (!state.outBuffer.empty()) {
    const std::size_t written = state.transportWrite(state.outBuffer, want);
    _stats.totalBytesWrittenFlush += written;
    switch (want) {
      case Transport::Error: {
        auto savedErr = errno;
        log::error("send/transportWrite failed fd={} errno={} msg={}", fd, savedErr, std::strerror(savedErr));
        state.requestImmediateClose();
        state.outBuffer.clear();
        break;
      }
      case Transport::ReadReady:
        [[fallthrough]];
      case Transport::WriteReady:
        [[fallthrough]];
      case Transport::None:
        if (written > 0) {
          if (written == state.outBuffer.remainingSize()) {
            state.outBuffer.clear();
            break;
          }
          state.outBuffer.addOffset(written);
          continue;
        }
        break;
    }
  }

  // Determine if we can drop EPOLLOUT: only when no buffered data AND no handshake wantWrite pending.
  if (state.outBuffer.empty() && state.waitingWritable && (state.tlsEstablished || state.transport->handshakeDone()) &&
      HttpServer::ModWithCloseOnFailure(_eventLoop, cnxIt, EPOLLIN | EPOLLET,
                                        "disable writable flushOutbound drop EPOLLOUT", _stats)) {
    state.waitingWritable = false;
    if (state.isAnyCloseRequested()) {
      return;
    }
  }
  // Clear writable interest if no buffered data and transport no longer needs write progress.
  // (We do not call handshakePending() here because ConnStateInternal does not expose it; transport has that.)
  if (state.outBuffer.empty()) {
    bool transportNeedsWrite = (!state.tlsEstablished && want == Transport::WriteReady);
    if (transportNeedsWrite) {
      if (!state.waitingWritable) {
        if (!HttpServer::ModWithCloseOnFailure(_eventLoop, cnxIt, EPOLLIN | EPOLLOUT | EPOLLET,
                                               "enable writable flushOutbound transportNeedsWrite", _stats)) {
          return;  // failure logged
        }
        state.waitingWritable = true;
      }
    } else if (state.waitingWritable) {
      state.waitingWritable = false;
      HttpServer::ModWithCloseOnFailure(_eventLoop, cnxIt, EPOLLIN | EPOLLET,
                                        "disable writable flushOutbound transport no longer needs", _stats);
    }
  }
}

}  // namespace aeronet
