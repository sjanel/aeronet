#include "aeronet/connection-state.hpp"

#include <sys/sendfile.h>
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
#include "aeronet/log.hpp"
#include "aeronet/protocol-handler.hpp"
#include "aeronet/transport.hpp"

#ifdef AERONET_ENABLE_OPENSSL
#include "aeronet/tls-config.hpp"
#include "aeronet/tls-handshake-callback.hpp"
#include "aeronet/tls-handshake.hpp"
#include "aeronet/tls-metrics.hpp"
#include "aeronet/tls-transport.hpp"
#endif

namespace aeronet {

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
  off_t off = static_cast<off_t>(fileSend.offset);

  ssize_t bytes;
  if (tlsFlow) {
    tunnelOrFileBuffer.ensureAvailableCapacityExponential(maxBytes);
    bytes = ::pread(fileSend.file.fd(), tunnelOrFileBuffer.data(), maxBytes, static_cast<off_t>(fileSend.offset));
  } else {
    bytes = ::sendfile(clientFd, fileSend.file.fd(), &off, maxBytes);
  }
  FileResult res{static_cast<std::size_t>(bytes), tlsFlow ? FileResult::Code::Read : FileResult::Code::Sent, tlsFlow};

  if (bytes == -1) [[unlikely]] {
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
    if (bytes > 0) {
      fileSend.offset = static_cast<std::size_t>(off);
      fileSend.remaining -= static_cast<std::size_t>(bytes);
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
    // No need to store ktlsSendEnabled separately - query tlsTr->isKtlsSendEnabled() when needed
  }

  return true;
}
#endif

void ConnectionState::reset() {
  // In order to avoid retaining large buffers in cached ConnectionState objects,
  // we shrink (before clear, otherwise it would free all memory) and clear them before reuse.
  tunnelOrFileBuffer.shrink_to_fit();
  inBuffer.shrink_to_fit();
  bodyAndTrailersBuffer.shrink_to_fit();
  headBuffer.shrink_to_fit();
  request.shrinkAndMaybeClear();
  outBuffer.shrink_to_fit();

  tunnelOrFileBuffer.clear();
  inBuffer.clear();
  bodyAndTrailersBuffer.clear();
  headBuffer.clear();
  // no need to clear request, it's built from scratch from initTrySetHead
  bodyStreamContext = {};
  outBuffer.clear();
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

  asyncState.clear();
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

void ConnectionState::AsyncHandlerState::clear() {
  if (handle) {
    handle.destroy();
    handle = {};
  }
  *this = {};
}

}  // namespace aeronet