#include "aeronet/connection-state.hpp"

#include <sys/sendfile.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <string_view>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response-data.hpp"
#include "aeronet/log.hpp"
#include "aeronet/transport.hpp"

namespace aeronet {

ITransport::TransportResult ConnectionState::transportRead(std::size_t chunkSize) {
  inBuffer.ensureAvailableCapacityExponential(chunkSize);

  const auto result = transport->read(inBuffer.data() + inBuffer.size(), chunkSize);
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
    } else if (bytes == 0) {
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
  std::string_view chunk(ctx->body.data() + ctx->offset, len);
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

void ConnectionState::clear() {
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
#if defined(AERONET_ENABLE_OPENSSL) && defined(AERONET_ENABLE_KTLS)
  ktlsSendAttempted = false;
  ktlsSendEnabled = false;
#endif
  waitingForBody = false;
  connectPending = false;
  tlsInfo = {};
#ifdef AERONET_ENABLE_OPENSSL
  handshakeStart = {};
#endif
  fileSend = {};

  asyncState.clear();
}

void ConnectionState::shrink_to_fit() {
  tunnelOrFileBuffer.shrink_to_fit();
  inBuffer.shrink_to_fit();
  bodyAndTrailersBuffer.shrink_to_fit();
  headBuffer.shrink_to_fit();
  request.shrink_to_fit();
  outBuffer.shrink_to_fit();
}

void ConnectionState::AsyncHandlerState::clear() {
  if (handle) {
    handle.destroy();
    handle = {};
  }
  *this = {};
}

}  // namespace aeronet