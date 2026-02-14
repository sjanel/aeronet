#include "aeronet/connection-state.hpp"

#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <string_view>
#include <utility>

#include "aeronet/file-payload.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response-data.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/log.hpp"
#include "aeronet/protocol-handler.hpp"
#include "aeronet/sendfile.hpp"
#include "aeronet/socket-ops.hpp"
#include "aeronet/transport.hpp"
#include "aeronet/zerocopy-mode.hpp"

#ifdef AERONET_ENABLE_OPENSSL
#include "aeronet/tls-config.hpp"
#include "aeronet/tls-handshake-callback.hpp"
#include "aeronet/tls-handshake.hpp"
#include "aeronet/tls-metrics.hpp"
#include "aeronet/tls-transport.hpp"
#endif

namespace aeronet {

void ConnectionState::initializeStateNewConnection(const HttpServerConfig& config, int cnxFd,
                                                   internal::ResponseCompressionState& compressionState) {
  request.init(config, compressionState);

  // Decide per-connection zerocopy preference at accept time.

  // Compute whether we should attempt to enable zerocopy for this connection.
  switch (config.zerocopyMode) {
    case ZerocopyMode::Disabled:
      zerocopyRequested = false;
      break;
    case ZerocopyMode::Enabled:
      zerocopyRequested = true;
      break;
    default: {
      assert(config.zerocopyMode == ZerocopyMode::Opportunistic);
      sockaddr_storage local{};
      sockaddr_storage peer{};
      const bool localOk = GetLocalAddress(cnxFd, local);
      const bool peerOk = GetPeerAddress(cnxFd, peer);

      // In Opportunistic mode, it's auto-disabled for loopback-to-loopback connections.
      const bool isLoopbackConn = localOk && peerOk && IsLoopback(local) && IsLoopback(peer);

      zerocopyRequested = !isLoopbackConn;
      break;
    }
  }
}

ITransport::TransportResult ConnectionState::transportRead(std::size_t chunkSize) {
  inBuffer.ensureAvailableCapacityExponential(chunkSize);

  const auto result = transport->read(inBuffer.data() + inBuffer.size(), inBuffer.availableCapacity());
  inBuffer.addSize(result.bytesProcessed);
  if (headerStartTp.time_since_epoch().count() == 0) {
    headerStartTp = std::chrono::steady_clock::now();
  }
  return result;
}

ITransport::TransportResult ConnectionState::transportWrite(std::string_view data) {
  const auto res = transport->write(data);
  if (!tlsEstablished && transport->handshakeDone()) {
    tlsEstablished = true;
  }
  return res;
}

ITransport::TransportResult ConnectionState::transportWrite(const HttpResponseData& httpResponseData) {
  const auto res = transport->write(httpResponseData.firstBuffer(), httpResponseData.secondBuffer());
  if (!tlsEstablished && transport->handshakeDone()) {
    tlsEstablished = true;
  }
  return res;
}

ConnectionState::FileResult ConnectionState::transportFile(int clientFd, bool tlsFlow) {
  static constexpr std::size_t kSendfileChunk = 1 << 16;

  const std::size_t maxBytes = std::min(fileSend.remaining, kSendfileChunk);
  off_t offset = static_cast<off_t>(fileSend.offset);

  int64_t result;
  if (tlsFlow) {
    tunnelOrFileBuffer.ensureAvailableCapacityExponential(maxBytes);
    result = static_cast<int64_t>(::pread(fileSend.file.fd(), tunnelOrFileBuffer.data(), maxBytes, offset));
  } else {
    result = Sendfile(clientFd, fileSend.file.fd(), offset, maxBytes);
  }
  FileResult res{static_cast<std::size_t>(result), tlsFlow ? FileResult::Code::Read : FileResult::Code::Sent, tlsFlow};

  if (result == -1) [[unlikely]] {
    res.bytesDone = 0;
    const int errnoVal = errno;
    static_assert(EAGAIN == EWOULDBLOCK, "Check logic below if EAGAIN != EWOULDBLOCK");
    switch (errnoVal) {
      case EWOULDBLOCK:
        res.enableWritable = true;
        [[fallthrough]];
      case EINTR:
        if (!tlsFlow || fileSend.remaining != 0) {
          res.code = FileResult::Code::WouldBlock;
        }
        return res;
      default:
        res.code = FileResult::Code::Error;
        log::error("{} failed during {} sendfile fd # {} errno={} msg={}", tlsFlow ? "pread" : "sendfile",
                   tlsFlow ? "TLS" : "plain", clientFd, errnoVal, std::strerror(errnoVal));
        requestImmediateClose();
        fileSend.active = false;
        return res;
    }
  }
  if (tlsFlow) {
    tunnelOrFileBuffer.setSize(res.bytesDone);

    // Update file send offsets according to bytes read.
    fileSend.offset += res.bytesDone;
    fileSend.remaining -= res.bytesDone;

    if (tunnelOrFileBuffer.empty() && fileSend.remaining == 0) {
      fileSend.active = false;
    }
  } else {
    // Successful transfer: update state based on the modified offset
    if (result > 0) {
      fileSend.offset = static_cast<std::size_t>(offset);
      fileSend.remaining -= static_cast<std::size_t>(result);
    } else {  // 0
      // sendfile() returning 0 with a non-blocking socket typically means the socket would block.
      // Treat it as WouldBlock to enable writable interest and wait for the socket to be ready.
      res.code = FileResult::Code::WouldBlock;
      res.enableWritable = true;
    }
    if (fileSend.remaining == 0) {
      fileSend.active = false;
      tunnelOrFileBuffer.clear();
    }
  }
  return res;
}

namespace {
std::string_view AggregateBufferedBody([[maybe_unused]] HttpRequest& request, void* context) {
  if (context == nullptr) {
    return {};
  }
  const auto* ctx = static_cast<const ConnectionState::AggregatedBodyStreamContext*>(context);
  return ctx->body;
}

std::string_view ReadBufferedBody([[maybe_unused]] HttpRequest& request, void* context, std::size_t maxBytes) {
  if (maxBytes == 0 || context == nullptr) {
    return {};
  }
  auto* ctx = static_cast<ConnectionState::AggregatedBodyStreamContext*>(context);
  if (ctx->offset >= ctx->body.size()) {
    return {};
  }
  const std::size_t remaining = ctx->body.size() - ctx->offset;
  const std::size_t len = std::min(maxBytes, remaining);
  const std::string_view chunk(ctx->body.data() + ctx->offset, len);
  ctx->offset += len;
  return chunk;
}

bool HasMoreBufferedBody([[maybe_unused]] const HttpRequest& request, void* context) {
  if (context == nullptr) {
    return false;
  }
  const auto* ctx = static_cast<const ConnectionState::AggregatedBodyStreamContext*>(context);
  return ctx->offset < ctx->body.size();
}
}  // namespace

void ConnectionState::installAggregatedBodyBridge() {
  if (request._bodyAccessBridge != nullptr) {
    return;
  }
  static constexpr HttpRequest::BodyAccessBridge kAggregatedBodyBridge{&AggregateBufferedBody, &ReadBufferedBody,
                                                                       &HasMoreBufferedBody};
  bodyStreamContext.body = request._body;
  bodyStreamContext.offset = 0;
  request._bodyAccessBridge = &kAggregatedBodyBridge;
  request._bodyAccessContext = &bodyStreamContext;
}
#ifdef AERONET_ENABLE_OPENSSL
bool ConnectionState::finalizeAndEmitTlsHandshakeIfNeeded(int fd, const TlsHandshakeCallback& cb,
                                                          TlsMetricsInternal& metrics, const TLSConfig& cfg) {
  auto* tlsTr = dynamic_cast<TlsTransport*>(transport.get());
  if (tlsTr == nullptr) {
    return false;
  }

  auto* ssl = tlsTr->rawSsl();

  tlsInfo =
      FinalizeTlsHandshake(ssl, fd, cfg.logHandshake, tlsHandshakeEventEmitted, cb, tlsInfo.handshakeStart, metrics);

  // SelectAlpn should abort the handshake (SSL_TLSEXT_ERR_ALERT_FATAL) before we reach here.
  assert(!tlsHandshakeObserver.alpnStrictMismatch && "ALPN strict mismatch should have aborted the handshake earlier");

  const auto ktlsMode = cfg.ktlsMode;
  if (ktlsMode != TLSConfig::KtlsMode::Disabled) {
    // Attempt to enable kTLS send offload. The transport caches the result so
    // isKtlsSendEnabled() can be queried later without re-checking the BIO.
    const auto application = MaybeEnableKtlsSend(tlsTr->enableKtlsSend(), fd, ktlsMode, metrics);
    if (application == KtlsApplication::CloseConnection) {
      requestImmediateClose();
    }

    // When kTLS send is enabled, we can use MSG_ZEROCOPY for large payloads.
    // This bypasses SSL_write and uses sendmsg() directly on the kTLS socket,
    // allowing the kernel to DMA from user pages directly to the NIC.
    if (zerocopyRequested && tlsTr->isKtlsSendEnabled()) {
      // Store the fd for direct socket I/O when using zerocopy.
      tlsTr->setUnderlyingFd(fd);
      tlsTr->enableZerocopy();
    }
  }

  return true;
}
#endif

void ConnectionState::reset() {
  // In order to avoid retaining large buffers in cached ConnectionState objects,
  // we shrink (before clear, otherwise it would free all memory) and clear them before reuse.
  const auto shrinkAndClear = [](auto& buffer) {
    buffer.shrink_to_fit();
    buffer.clear();
  };
  shrinkAndClear(inBuffer);
  shrinkAndClear(bodyAndTrailersBuffer);
#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  shrinkAndClear(asyncState.headBuffer);
#endif
  shrinkAndClear(tunnelOrFileBuffer);

  request.shrinkAndMaybeClear();

  shrinkAndClear(outBuffer);
  // no need to clear request, it's built from scratch from initTrySetHead
  bodyStreamContext = {};
  transport.reset();
  lastActivity = std::chrono::steady_clock::now();
  headerStartTp = {};
  bodyLastActivity = {};
  peerFd = -1;
  requestsServed = 0;
  trailerStartPos = 0;
  closeMode = CloseMode::None;
  waitingWritable = false;
  tlsEstablished = false;
  waitingForBody = false;
  connectPending = false;
  tlsInfo = {};
#ifdef AERONET_ENABLE_OPENSSL
  tlsHandshakeObserver = {};
  tlsHandshakeEventEmitted = false;
  tlsContextKeepAlive.reset();
  tlsHandshakeInFlight = false;
#endif
  fileSend = {};

  // Reset protocol handler (e.g., WebSocket, HTTP/2)
  protocolHandler.reset();
  protocol = ProtocolType::Http11;

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  asyncState.clear();
#endif
}

bool ConnectionState::attachFilePayload(FilePayload filePayload) {
  fileSend.file = std::move(filePayload.file);
  fileSend.offset = filePayload.offset;
  fileSend.remaining = filePayload.length;
  fileSend.active = fileSend.remaining > 0;
  fileSend.headersPending = !outBuffer.empty();
  if (isSendingFile()) {
    // Don't enable writable interest here - let flushFilePayload do it when it actually blocks.
    // Enabling it prematurely (when the socket is already writable) causes us to miss the edge
    // in edge-triggered epoll mode.
    if (!fileSend.headersPending) {
      return true;
    }
  }
  return false;
}

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
void ConnectionState::AsyncHandlerState::clear() {
  if (handle) {
    handle.destroy();
    handle = {};
  }
  *this = {};
}
#endif

}  // namespace aeronet