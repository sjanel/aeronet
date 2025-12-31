#include "aeronet/http2-config.hpp"

#include <stdexcept>

namespace aeronet {

void Http2Config::validate() const {
  // SETTINGS_MAX_FRAME_SIZE must be between 16384 and 16777215 (RFC 9113 §6.5.2)
  if (maxFrameSize < 16384 || maxFrameSize > 16777215) {
    throw std::invalid_argument("Http2Config: maxFrameSize must be between 16384 and 16777215");
  }

  // SETTINGS_INITIAL_WINDOW_SIZE must not exceed 2^31-1 (RFC 9113 §6.5.2)
  if (initialWindowSize > 2147483647) {
    throw std::invalid_argument("Http2Config: initialWindowSize must not exceed 2147483647");
  }

  // Connection window size should be reasonable
  if (connectionWindowSize > 2147483647) {
    throw std::invalid_argument("Http2Config: connectionWindowSize must not exceed 2147483647");
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
}

}  // namespace aeronet
