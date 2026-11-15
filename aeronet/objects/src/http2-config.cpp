#include "aeronet/http2-config.hpp"

#include <chrono>
#include <stdexcept>

namespace aeronet {

namespace {

// 0 would make a timer expire at once: no value disables it, std::chrono::milliseconds::max() never expires.
void CheckPositive(std::chrono::milliseconds duration, const char* errorMessage) {
  if (duration.count() <= 0) {
    throw std::invalid_argument(errorMessage);
  }
}

}  // namespace

void Http2Config::validate() const {
  // SETTINGS_MAX_FRAME_SIZE must be between 16384 and 16777215 (RFC 9113 §6.5.2)
  if (maxFrameSize < 16384 || maxFrameSize > 16777215) {
    throw std::invalid_argument("Http2Config: maxFrameSize must be between 16384 and 16777215");
  }

  // SETTINGS_INITIAL_WINDOW_SIZE must not exceed 2^31-1 (RFC 9113 §6.5.2). A window is only replenished up to this
  // size: with 0, a stream could never receive any DATA.
  if (initialWindowSize == 0 || initialWindowSize > 2147483647) {
    throw std::invalid_argument("Http2Config: initialWindowSize must be between 1 and 2147483647");
  }

  // The connection window starts at 65535 bytes for both endpoints and no frame can reduce it (RFC 9113 §6.9.2).
  if (connectionWindowSize < 65535 || connectionWindowSize > 2147483647) {
    throw std::invalid_argument("Http2Config: connectionWindowSize must be between 65535 and 2147483647");
  }

  // Header table size should be reasonable (0 is valid - disables dynamic table)
  // No upper limit specified in RFC, but we cap at 64KB for sanity
  if (headerTableSize > 65536) {
    throw std::invalid_argument("Http2Config: headerTableSize should not exceed 65536");
  }

  // Max concurrent streams of 0 is valid (peer cannot open streams)
  // No validation needed

  // Max header list size should be reasonable
  if (maxHeaderListSize == 0) {
    throw std::invalid_argument("Http2Config: maxHeaderListSize must be greater than 0");
  }

  // Priority tree depth should be reasonable
  if (maxPriorityTreeDepth == 0) {
    throw std::invalid_argument("Http2Config: maxPriorityTreeDepth must be greater than 0");
  }

  if (maxStreamPendingBytes == 0) {
    throw std::invalid_argument("Http2Config: maxStreamPendingBytes must be greater than 0");
  }

  CheckPositive(settingsTimeout, "Http2Config: settingsTimeout must be positive");
  CheckPositive(pingInterval, "Http2Config: pingInterval must be positive");
  CheckPositive(pingTimeout, "Http2Config: pingTimeout must be positive");
  CheckPositive(streamIdleTimeout, "Http2Config: streamIdleTimeout must be positive");
}

}  // namespace aeronet
