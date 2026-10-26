#pragma once

#include <memory>
#include <type_traits>

namespace aeronet::websocket {

/// zlib streams shared by the WebSocket connections of an event loop (a server thread), for the directions negotiated
/// without context takeover (server_no_context_takeover, client_no_context_takeover): their state does not survive a
/// message, so one stream serves all these connections. This saves the memory of a stream per connection (about 300 KB
/// for compression) and keeps the shared one in the CPU caches. Not thread-safe: only used by its event loop.
class SharedDeflateStreams {
 public:
  SharedDeflateStreams() noexcept;

  SharedDeflateStreams(const SharedDeflateStreams&) = delete;
  SharedDeflateStreams& operator=(const SharedDeflateStreams&) = delete;
  SharedDeflateStreams(SharedDeflateStreams&&) noexcept;
  SharedDeflateStreams& operator=(SharedDeflateStreams&&) noexcept;

  ~SharedDeflateStreams();

  using trivially_relocatable = std::true_type;

 private:
  friend class DeflateContext;

  struct Impl;

  // On the heap, created on first use: the connections keep pointers to the streams, and a zlib stream cannot move.
  // Only followed when a connection sets up its DeflateContext, which then points to the streams themselves.
  std::unique_ptr<Impl> _impl;
};

}  // namespace aeronet::websocket
