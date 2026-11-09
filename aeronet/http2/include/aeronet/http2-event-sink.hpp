#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "aeronet/http2-frame-types.hpp"

namespace aeronet::http2 {

struct DecodedHeaders;

/// Observer for connection-level events. At most one sink is attached per connection
/// (1:1 with the owning protocol handler or client engine).
/// A callback may send on, reset or finalize streams of the connection, but must not open a new one: the connection
/// keeps an iterator to the stream of the frame being processed across its callbacks.
class EventSink {
 public:
  virtual ~EventSink() = default;

  /// A header block was decoded on a stream. The sink may take its headers over (see DecodedHeaders): they are only
  /// valid until the next header block is decoded otherwise.
  virtual void onHeadersDecoded(uint32_t streamId, DecodedHeaders& decodedHeaders, bool endStream) = 0;

  virtual void onData(uint32_t streamId, std::span<const std::byte> data, bool endStream) = 0;

  virtual void onStreamReset(uint32_t streamId, ErrorCode errorCode) = 0;

  virtual void onStreamClosed(uint32_t streamId) = 0;

  virtual void onGoAway(uint32_t lastStreamId, ErrorCode errorCode, std::string_view debugData) = 0;

  virtual void onWindowUpdate(uint32_t streamId, uint32_t increment) = 0;
};

}  // namespace aeronet::http2