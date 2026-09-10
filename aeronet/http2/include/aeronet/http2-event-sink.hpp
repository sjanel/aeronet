#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "aeronet/http2-frame-types.hpp"
#include "aeronet/sv-to-sv-map.hpp"

namespace aeronet::http2 {

/// Observer for connection-level events. At most one sink is attached per connection
/// (1:1 with the owning protocol handler or client engine).
class EventSink {
 public:
  virtual ~EventSink() = default;

  virtual void onHeadersDecoded([[maybe_unused]] uint32_t streamId, [[maybe_unused]] const SvToSvMap& headers,
                                bool /*endStream*/) = 0;
  virtual void onData([[maybe_unused]] uint32_t streamId, [[maybe_unused]] std::span<const std::byte> data,
                      [[maybe_unused]] bool endStream) = 0;
  virtual void onStreamReset([[maybe_unused]] uint32_t streamId, [[maybe_unused]] ErrorCode errorCode) = 0;
  virtual void onStreamClosed([[maybe_unused]] uint32_t streamId) = 0;
  virtual void onGoAway([[maybe_unused]] uint32_t lastStreamId, [[maybe_unused]] ErrorCode errorCode,
                        [[maybe_unused]] std::string_view debugData) = 0;
  virtual void onWindowUpdate([[maybe_unused]] uint32_t streamId, [[maybe_unused]] uint32_t increment) = 0;
};

}  // namespace aeronet::http2