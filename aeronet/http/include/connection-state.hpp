#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

#include "aeronet/http-response-data.hpp"
#include "file.hpp"
#include "raw-chars.hpp"
#include "tls-info.hpp"
#include "transport.hpp"

namespace aeronet {

struct ConnectionState {
  [[nodiscard]] bool isImmediateCloseRequested() const noexcept { return closeMode == CloseMode::Immediate; }
  [[nodiscard]] bool isDrainCloseRequested() const noexcept { return closeMode == CloseMode::DrainThenClose; }
  [[nodiscard]] bool isAnyCloseRequested() const noexcept { return closeMode != CloseMode::None; }

  [[nodiscard]] bool isTunneling() const noexcept { return peerFd != -1; }
  [[nodiscard]] bool isSendingFile() const noexcept { return fileSend.active; }

  // Request to close immediately (abort outstanding buffered writes).
  void requestImmediateClose() { closeMode = CloseMode::Immediate; }

  // Request to close after draining currently buffered writes (graceful half-close semantics).
  void requestDrainAndClose() {
    if (closeMode == CloseMode::None) {
      closeMode = CloseMode::DrainThenClose;
    }
  }

  ITransport::TransportResult transportRead(std::size_t chunkSize);

  ITransport::TransportResult transportWrite(std::string_view data);
  ITransport::TransportResult transportWrite(const HttpResponseData& httpResponseData);

  // Result of a kernel sendfile operation performed on this connection's fileSend state.
  struct FileResult {
    enum class Code : uint8_t { Read, Sent, WouldBlock, Error };

    std::size_t bytesDone{0};
    Code code{Code::Sent};
    // When code == WouldBlock, indicates the caller should enable writable interest
    // (true for EAGAIN/EWOULDBLOCK, false for EINTR).
    bool enableWritable{false};
  };

  // if tls is false:
  //   Attempt to send up to maxChunk bytes from the currently tracked file via the kernel
  //   sendfile(2) syscall. The method updates fileSend.offset and fileSend.remaining on
  //   successful transfers. It does NOT modify EPOLL interest; the caller should consult
  //   the returned SendfileResult and invoke enableWritableInterest/disableWritableInterest
  //   as appropriate.
  // if tls is true:
  //   Read up to `maxBytes` from the tracked file into `tunnelOrFileBuffer`. The method
  //   will not request EPOLL changes or log; it simply fills the buffer and returns a
  //   structured result so callers can decide on logging/closing/enabling writable interest.
  FileResult transportFile(int clientFd, bool tlsFlow);

  // Buffer used for tunneling raw bytes when peer is not writable, or for send file buffer (they are both mutually
  // exclusive).
  RawChars tunnelOrFileBuffer;
  RawChars inBuffer;                      // accumulated input raw data
  RawChars bodyAndTrailersBuffer;         // decoded body + optional trailer headers (RFC 7230 §4.1.2)
  HttpResponseData outBuffer;             // pending outbound data not yet written
  std::unique_ptr<ITransport> transport;  // set after accept (plain or TLS)
  std::chrono::steady_clock::time_point lastActivity{std::chrono::steady_clock::now()};
  // Timestamp of first byte of the current pending request headers (buffer not yet containing full CRLFCRLF).
  // Reset when a complete request head is parsed. If std::chrono::steady_clock::time_point{} (epoch) -> inactive.
  std::chrono::steady_clock::time_point headerStart;  // default epoch value means no header timing active
  // Tunnel support: when a connection is acting as a tunnel endpoint, peerFd holds the
  // file descriptor of the other side (upstream or client).
  int peerFd{-1};
  uint32_t requestsServed{0};
  // Position where trailer headers start in bodyAndTrailersBuffer (0 if no trailers).
  // Trailers occupy [trailerStartPos, bodyAndTrailersBuffer.size()).
  std::size_t trailerStartPos{0};
  // Connection close lifecycle.
  enum class CloseMode : uint8_t { None, DrainThenClose, Immediate };

  CloseMode closeMode{CloseMode::None};
  bool waitingWritable{false};  // EPOLLOUT registered
  bool tlsEstablished{false};   // true once TLS handshake completed (if TLS enabled)
  // Tunnel state: true when peerFd != -1. Use accessor isTunneling() to query.
  // True when a non-blocking connect() was issued and completion is pending (EPOLLOUT will signal).
  bool connectPending{false};
  TLSInfo tlsInfo;
  std::chrono::steady_clock::time_point handshakeStart;  // TLS handshake start time (steady clock)

  struct FileSendState {
    File file;
    bool active{false};
    bool headersPending{false};
    std::size_t offset{0};
    std::size_t remaining{0};
  };

  FileSendState fileSend;
};

}  // namespace aeronet