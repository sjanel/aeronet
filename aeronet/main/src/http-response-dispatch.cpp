#include <algorithm>
#include <cerrno>
#include <chrono>  // steady_clock for metrics duration
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
#include "aeronet/http-version.hpp"  // http::HTTP_1_0 / HTTP_1_1
#include "connection-state.hpp"      // ConnectionState
#include "log.hpp"
#include "raw-chars.hpp"
#include "string-equal-ignore-case.hpp"

namespace aeronet {
void HttpServer::finalizeAndSendResponse(int fd, ConnectionState& state, HttpRequest& req, HttpResponse& resp,
                                         std::size_t consumedBytes, std::chrono::steady_clock::time_point reqStart) {
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
    auto encHeader = req.headerValueOrEmpty(http::AcceptEncoding);
    auto [encoding, reject] = _encodingSelector.negotiateAcceptEncoding(encHeader);
    // If the client explicitly forbids identity (identity;q=0) and we have no acceptable
    // alternative encodings to offer, emit a 406 per RFC 9110 Section 12.5.3 guidance.
    if (reject) {
      resp.statusCode(406)
          .reason(http::ReasonNotAcceptable)
          .body("No acceptable content-coding available")
          .contentType(http::ContentTypeTextPlain);
    }
    // Apply size threshold for non-streaming (buffered) responses: if body below minBytes skip compression.
    else if (encoding != Encoding::none && resp.body().size() < compressionConfig.minBytes) {
      encoding = Encoding::none;
    }
    // Approximate allowlist check (default text/plain assumption until header getter exists)
    if (!compressionConfig.contentTypeAllowlist.empty()) {
      std::string_view assumed = "text/plain";
      bool ok = false;
      for (const auto& prefix : compressionConfig.contentTypeAllowlist) {
        if (assumed.starts_with(prefix)) {
          ok = true;
          break;
        }
      }
      if (!ok) {
        encoding = Encoding::none;
      }
    }
    if (encoding != Encoding::none) {
      auto& encoder = _encoders[static_cast<size_t>(encoding)];
      if (encoder) {
        auto out = encoder->encodeFull(resp.body());
        resp.customHeader(http::ContentEncoding, GetEncodingStr(encoding));
        if (compressionConfig.addVaryHeader) {
          resp.customHeader(http::Vary, http::AcceptEncoding);
        }
        resp.body(out);
      }
    }
  }
  auto data = resp.finalizeAndGetFullTextResponse(req.version(), _cachedDateEpoch, keepAlive, isHead);

  queueData(fd, state, data);

  state.buffer.erase_front(consumedBytes);
  if (!keepAlive) {
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

bool HttpServer::queueData(int fd, ConnectionState& state, std::string_view data) {
  if (state.outBuffer.empty()) {
    bool wantR = false;
    bool wantW = false;
    auto written = transportWrite(fd, state, data, wantR, wantW);
    state.tlsWantRead = wantR;
    state.tlsWantWrite = wantW;
    if (!state.tlsEstablished && state.transport && !state.transport->handshakePending()) {
      state.tlsEstablished = true;
    }
    if (written > 0) {
      if (std::cmp_equal(written, data.size())) {
        _stats.totalBytesQueued += static_cast<uint64_t>(data.size());
        _stats.totalBytesWrittenImmediate += static_cast<uint64_t>(written);
        // If TLS wants more write (handshake) and no buffered data, ensure EPOLLOUT
        if (wantW && !state.waitingWritable) {
          if (!HttpServer::ModWithCloseOnFailure(_loop, fd, EPOLLIN | EPOLLOUT | EPOLLET, state,
                                                 "enable writable immediate write path", _stats)) {
            return false;
          }
          state.waitingWritable = true;
          ++_stats.deferredWriteEvents;
        }
        return true;
      }
      // partial write
      _stats.totalBytesWrittenImmediate += static_cast<uint64_t>(written);
      state.outBuffer.append(data.data() + written, data.data() + data.size());
    } else if (written == 0 || (written == -1 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))) {
      // no progress but non fatal
      state.outBuffer.append(data);
    } else {  // fatal transport write
      state.requestImmediateClose();
      return false;
    }
  } else {
    state.outBuffer.append(data);
  }
  _stats.totalBytesQueued += static_cast<uint64_t>(data.size());
  _stats.maxConnectionOutboundBuffer = std::max(_stats.maxConnectionOutboundBuffer, state.outBuffer.size());
  if (state.outBuffer.size() > _config.maxOutboundBufferBytes) {
    state.requestImmediateClose();
  }
  if (!state.waitingWritable) {
    if (HttpServer::ModWithCloseOnFailure(_loop, fd, EPOLLIN | EPOLLOUT | EPOLLET, state,
                                          "enable writable buffered path", _stats)) {
      state.waitingWritable = true;
      ++_stats.deferredWriteEvents;
    }
  }
  return true;
}

void HttpServer::flushOutbound(int fd, ConnectionState& state) {
  ++_stats.flushCycles;
  bool lastWantWrite = false;
  while (!state.outBuffer.empty()) {
    bool wantR = false;
    bool wantW = false;
    auto written = transportWrite(fd, state, state.outBuffer, wantR, wantW);
    lastWantWrite = wantW;
    state.tlsWantRead = wantR;
    state.tlsWantWrite = wantW;
    if (!state.tlsEstablished && state.transport && !state.transport->handshakePending()) {
      state.tlsEstablished = true;
    }
    if (written > 0) {
      _stats.totalBytesWrittenFlush += static_cast<uint64_t>(written);
      if (std::cmp_equal(written, state.outBuffer.size())) {
        state.outBuffer.clear();
        break;
      }
      state.outBuffer.erase_front(static_cast<std::size_t>(written));
      continue;
    }
    if (written == 0 || (written == -1 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))) {
      // Need to wait for socket writable again (TLS handshake or congestion)
      break;
    }
    int savedErr = errno;
    log::error("send/transportWrite failed fd={} errno={} msg={}", fd, savedErr, std::strerror(savedErr));
    state.requestImmediateClose();
    state.outBuffer.clear();
    break;
  }
  // Determine if we can drop EPOLLOUT: only when no buffered data AND no handshake wantWrite pending.
  if (state.outBuffer.empty() && state.waitingWritable) {
    bool keepWritable = false;
    if (!state.tlsEstablished && state.transport && state.transport->handshakePending()) {
      keepWritable = true;
    }
    if (!keepWritable) {
      if (HttpServer::ModWithCloseOnFailure(_loop, fd, EPOLLIN | EPOLLET, state,
                                            "disable writable flushOutbound drop EPOLLOUT", _stats)) {
        state.waitingWritable = false;
        if (state.isAnyCloseRequested()) {
          closeConnectionFd(fd);
        }
      }
    }
  }
  // Clear writable interest if no buffered data and transport no longer needs write progress.
  // (We do not call handshakePending() here because ConnStateInternal does not expose it; transport has that.)
  if (state.outBuffer.empty()) {
    bool transportNeedsWrite = (!state.tlsEstablished && (state.tlsWantWrite || lastWantWrite));
    if (transportNeedsWrite) {
      if (!state.waitingWritable) {
        if (!HttpServer::ModWithCloseOnFailure(_loop, fd, EPOLLIN | EPOLLOUT | EPOLLET, state,
                                               "enable writable flushOutbound transportNeedsWrite", _stats)) {
          return;  // failure logged
        }
        state.waitingWritable = true;
      }
    } else if (state.waitingWritable) {
      state.waitingWritable = false;
      HttpServer::ModWithCloseOnFailure(_loop, fd, EPOLLIN | EPOLLET, state,
                                        "disable writable flushOutbound transport no longer needs", _stats);
    }
  }
}

}  // namespace aeronet
