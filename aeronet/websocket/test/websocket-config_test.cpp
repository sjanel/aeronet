#include "aeronet/websocket-config.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <limits>
#include <stdexcept>

namespace aeronet::websocket {

TEST(WebSocketConfigTest, DefaultIsValid) { EXPECT_NO_THROW(WebSocketConfig{}.validate()); }

TEST(WebSocketConfigTest, ZeroMaxMessageSizeIsInvalid) {
  WebSocketConfig config;
  config.maxMessageSize = 0;
  EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(WebSocketConfigTest, ZeroMaxFrameSizeIsInvalid) {
  WebSocketConfig config;
  config.maxFrameSize = 0;
  EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(WebSocketConfigTest, SmallestLimitsAreValid) {
  WebSocketConfig config;
  config.maxMessageSize = 1;
  config.maxFrameSize = 1;
  EXPECT_NO_THROW(config.validate());
}

TEST(WebSocketConfigTest, LargestLimitsApproximateNoLimit) {
  WebSocketConfig config;
  config.maxMessageSize = std::numeric_limits<std::size_t>::max();
  config.maxFrameSize = std::numeric_limits<std::size_t>::max();
  EXPECT_NO_THROW(config.validate());
}

TEST(WebSocketConfigTest, InvalidDeflateConfigIsInvalid) {
  WebSocketConfig config;
  config.deflateConfig.compressionLevel = 10;
  EXPECT_THROW(config.validate(), std::invalid_argument);
}

}  // namespace aeronet::websocket
