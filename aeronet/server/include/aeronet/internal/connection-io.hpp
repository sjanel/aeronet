#pragma once

#include <amc/type_traits.hpp>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

#include "aeronet/connection-state.hpp"
#include "aeronet/http-message-data.hpp"
#include "aeronet/internal/connection-storage.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/transport-result.hpp"
#include "aeronet/vector.hpp"
#include "aeronet/zerocopy.hpp"

namespace aeronet::internal {

struct ServerContext;

// Whether a connection must be closed once the handling of its event completes.
enum class CloseStatus : uint8_t { Keep, Close };

// The I/O of the connections of a SingleHttpServer, shared by all the protocols: the output (queued behind the output
// waiting for the socket, gathered, written with MSG_ZEROCOPY when eligible, files sent with sendfile() or encrypted
// in user space), the writable interest, the input buffers lent to the reads, the input deferred to the end of the
// event loop iteration, and the close of the connections.
class ConnectionIo {
 public:
  using ConnectionIt = ConnectionStorage::ConnectionIt;

  explicit ConnectionIo(ServerContext& ctx) noexcept : _pCtx(&ctx) {}

  // Points to the context of the server owning this component, after a move of that context.
  void bind(ServerContext& ctx) noexcept { _pCtx = &ctx; }

  // Queues the data on the connection, writing it right away when no output is waiting for the socket. On transport
  // failure, the connection is closed once its output is dropped.
  void queueData(ConnectionIt cnxIt, HttpMessageData httpResponseData);

  // Writes the pending output of the connection: its HTTP/1 output (with its file payloads), then the output of its
  // protocol handler, until the socket would block.
  void flushOutbound(ConnectionIt cnxIt);

  // The connection of the HTTP/1.1 request followed by other requests in its input buffer, while they are served
  // (nullptr otherwise): queueData() does not write their responses right away, but keeps them to write them
  // together, in a single write, once they are served. Responding to each pipelined request with its own write would
  // cost a syscall per request and, with Nagle's algorithm, delay the responses after the first one until the client
  // acknowledges it.
  void batchOutputOf(ConnectionState* pState) noexcept { _pOutputBatch = pState; }

  // Enable / disable the writable interest (EPOLLOUT) of a connection, updating ConnectionState::waitingWritable.
  // Return true on success, false on failure: the output of the connection is then dropped and it closes.
  bool enableWritableInterest(ConnectionIt cnxIt);
  bool disableWritableInterest(ConnectionIt cnxIt);

  // Forgets the writable interest of a connection (no event loop update).
  void forgetWritableInterest(ConnectionState& state) noexcept {
    if (state.waitingWritable) {
      state.waitingWritable = false;
      assert(_nbWritableConnections > 0U);
      --_nbWritableConnections;
    }
  }

  // Number of connections waiting for writable events.
  [[nodiscard]] uint32_t nbWritableConnections() const noexcept { return _nbWritableConnections; }

  // Resumes the input of the connection at the end of the event loop iteration (see takeDeferredInputs()), once: those
  // that hit the per-event fairness cap with data still in their TCP buffer (edge-triggered polling - EPOLLET on Linux,
  // EV_CLEAR on macOS - only fires on state transitions), or requests in their input buffer, or records read ahead by
  // their TLS transport.
  void deferInput(NativeHandle fd, ConnectionState& state);

  [[nodiscard]] bool hasDeferredInputs() const noexcept { return !_pendingReadFds.empty(); }

  // The fds whose input was deferred, leaving none: the input of a resumed connection can be deferred again.
  [[nodiscard]] vector<NativeHandle> takeDeferredInputs() noexcept { return std::exchange(_pendingReadFds, {}); }

  // Resumes the input processing stopped by the output backpressure (see ConnectionState::inputBlockedByOutput) once
  // the output and file payload drained.
  void resumeInputIfOutputDrained(NativeHandle fd, ConnectionState& state);

  // A connection without pending input reads into the input buffer of the event loop instead of its own: the memory of
  // the previous such read, still in the CPU caches. Does nothing if it has pending input.
  void lendInBuffer(ConnectionState& state) noexcept {
    if (state.inBuffer.empty()) {
      // The empty buffer of the connection (without capacity, unless it once kept pending input) waits in its place.
      state.inBuffer.swap(_lentInBuffer);
    }
  }

  // Once the input of a connection is entirely served, its input buffer is not needed until its next read: the larger
  // of it and the input buffer of the event loop becomes the latter, the other is freed. An idle connection holds no
  // input buffer. A connection with pending input keeps its buffer.
  void releaseServedInBuffer(ConnectionState& state) noexcept {
    // Pending input (a partial request or frame, a request held by an async handler or waiting for its CONNECT
    // target...) is kept where it is, along with the views pointing to it. Nothing points to an empty buffer.
    if (!state.inBuffer.empty()) {
      return;
    }
    // On a tie, the buffer just used, still in the CPU caches, is kept.
    if (_lentInBuffer.capacity() <= state.inBuffer.capacity()) {
      state.inBuffer.swap(_lentInBuffer);
    }
    if (state.inBuffer.capacity() != 0) {
      state.inBuffer = RawChars{};
    }
  }

  // Frees the input buffer of the event loop (between two events, it is not lent), to avoid unbounded memory growth.
  void shrinkLentInBuffer() noexcept { _lentInBuffer.shrink_to_fit(); }

  // Closes the connection, unless it starts a lingering close (see startLingeringClose()).
  void closeConnection(ConnectionIt cnxIt);

  // Closes the connection right away, with the other end of its tunnels.
  void closeConnectionNow(ConnectionIt cnxIt);

  // Unregister the connection from the event loop and release its state (closing its socket). Unlike
  // closeConnection(), neither its maintenance nor its tunnels are handled.
  void releaseConnection(ConnectionIt cnxIt);

  // Closes all the connections, and the tunnels with them.
  void closeAllConnections();

  // Reads and discards the input of a lingering connection. Returns Close once the client closed it.
  CloseStatus discardLingeringInput(ConnectionIt cnxIt);

#ifdef AERONET_LINUX
  // Polls the zerocopy completion notifications of a connection (see ConnectionState::pollZerocopyCompletions()).
  ZerocopyCompletions pollZerocopyCompletions(ConnectionState& state);

  // Releases the buffers of the zerocopy sends of reset connections whose grace period elapsed.
  void releaseResetZerocopyBuffers(std::chrono::steady_clock::time_point now);

  // Waits for the end of the grace period of the buffers of the zerocopy sends of reset connections, then releases
  // them.
  void waitForResetZerocopyBuffers();

  // Whether the listening socket has SO_ZEROCOPY, inherited by the accepted sockets.
  [[nodiscard]] bool zerocopySupported() const noexcept { return _zerocopy.supported; }

  void setZerocopySupported(bool supported) noexcept { _zerocopy.supported = supported; }
#endif

 private:
  // Writes the unsent bytes of 'data' to the connection, with MSG_ZEROCOPY when eligible (Linux): sets 'zerocopySent'
  // in that case, the kernel then reading the buffers of 'data' until the completion of the send (see
  // ConnectionState::retainZerocopyBuffer()).
  TransportResult writeMessage(ConnectionIt cnxIt, const HttpMessageData& data, bool& zerocopySent);

  // Same for the unsent bytes of messages, as 'buffers' (at most ZeroCopyState::kMaxBuffers), of the sizes in 'output'.
  TransportResult writeMessages(ConnectionIt cnxIt, std::span<const std::string_view> buffers,
                                const ConnectionState::GatheredOutput& output, bool& zerocopySent);

  // Writes outBuffer, gathered with the queued entries that follow it (up to the next file payload, sent before what
  // follows it). Always gathered, so that a write retried after it would block starts with the same bytes, at least as
  // many, as user-space TLS requires (OpenSSL keeps the record it could not write, and fails a retry with fewer bytes).
  // Returns the number of bytes written, 'want' being set to the hint of the transport.
  std::size_t writeOutBuffer(ConnectionIt cnxIt, TransportHint& want);

  void flushFilePayload(ConnectionIt cnxIt);

  // Flushes the pending bytes of tunnelOrFileBuffer via user-space TLS (SSL_write), when kTLS is not available and file
  // data must be encrypted in user space. Returns true if the caller should return early: the buffer is still
  // non-empty (the write would block), or its write failed (the file send is then dropped).
  bool flushUserSpaceTlsBuffer(ConnectionIt cnxIt);

  // RFC 9112 §9.6: closing a socket with unread input resets the connection, and the reset can destroy the response
  // before the client reads it (Windows discards the received data). When the client of an HTTP/1.1 connection whose
  // drain close is due may still be sending (bytes of the answered request, or of requests pipelined behind it, are
  // left in its input buffer), shuts down its write side and keeps it open, discarding its input, until the client
  // closes it or lingeringCloseTimeout elapsed. Returns true if the connection lingers.
  bool startLingeringClose(ConnectionIt cnxIt);

  // Largest output batched for a connection by queueData() (see batchOutputOf()): a response that would exceed it is
  // written right away, along with the batched ones.
  static constexpr std::size_t kMaxBatchedOutputBytes = std::size_t{64} << 10U;

#ifdef AERONET_LINUX
  // A server stops using MSG_ZEROCOPY for this long after a send failed with ENOBUFS.
  static constexpr std::chrono::seconds kZerocopyNoBufferSpaceBackoff{1};
  // Buffers of the zerocopy sends of a reset connection are kept this long: the reset dropped the socket send queue,
  // but the packets already handed to the network device (queuing discipline, NIC ring) may still reference them.
  static constexpr std::chrono::seconds kZerocopyResetGracePeriod{1};

  struct ZerocopyRuntime {
    // A buffer of the zerocopy sends of a reset connection, released once releaseTime elapsed.
    struct ResetBuffer {
      using trivially_relocatable = amc::is_trivially_relocatable<HttpMessageData>::type;

      HttpMessageData data;
      std::chrono::steady_clock::time_point releaseTime;
    };

    // In release time order.
    vector<ResetBuffer> resetBuffers;
    // No zerocopy send before this time (after ENOBUFS).
    std::chrono::steady_clock::time_point retryTime;
    // Whether the listening socket has SO_ZEROCOPY, inherited by the accepted sockets.
    bool supported{false};
    // Whether ENOBUFS was already logged (once per server).
    bool noBufferSpaceLogged{false};
  };
#endif

  ServerContext* _pCtx;
  // See batchOutputOf().
  ConnectionState* _pOutputBatch{nullptr};
  // See deferInput().
  vector<NativeHandle> _pendingReadFds;
  // Input buffer of the event loop, lent to the reads of the connections that have no pending input (see
  // lendInBuffer()).
  RawChars _lentInBuffer;
#ifdef AERONET_LINUX
  ZerocopyRuntime _zerocopy;
#endif
  uint32_t _nbWritableConnections{0};
};

}  // namespace aeronet::internal
