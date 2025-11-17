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

#include "aeronet/http-response-data.hpp"
#include "aeronet/log.hpp"
#include "aeronet/transport.hpp"

namespace aeronet {

ITransport::TransportResult ConnectionState::transportRead(std::size_t chunkSize) {
  inBuffer.ensureAvailableCapacityExponential(chunkSize);

  const auto result = transport->read(inBuffer.data() + inBuffer.size(), chunkSize);
  inBuffer.addSize(result.bytesProcessed);
  if (headerStart.time_since_epoch().count() == 0) {
    headerStart = std::chrono::steady_clock::now();
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

  if (bytes < 0) {
    res.bytesDone = 0;
    int errnoVal = errno;
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

}  // namespace aeronet