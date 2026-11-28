#include "aeronet/websocket-config.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <limits>
#include <stdexcept>

#include "aeronet/chrono-lite.hpp"

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

TEST(WebSocketConfigTest, IdleTimeoutMustBePositive) {
  WebSocketConfig config;
  config.idleTimeout = std::chrono::milliseconds{0};
  EXPECT_THROW(config.validate(), std::invalid_argument);
  config.idleTimeout = std::chrono::milliseconds{-1};
  EXPECT_THROW(config.validate(), std::invalid_argument);
  config.idleTimeout = std::chrono::milliseconds::max();  // no idle timeout
  EXPECT_NO_THROW(config.validate());
}

TEST(WebSocketConfigTest, CloseTimeoutMustNotBeNegative) {
  WebSocketConfig config;
  config.closeTimeout = std::chrono::milliseconds{-1};
  EXPECT_THROW(config.validate(), std::invalid_argument);
  config.closeTimeout = std::chrono::milliseconds{0};  // closed once the Close frame is sent
  EXPECT_NO_THROW(config.validate());
  config.closeTimeout = std::chrono::milliseconds::max();  // waits for the peer forever
  EXPECT_NO_THROW(config.validate());
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

#ifdef AERONET_ENABLE_ZLIB
TEST(WebSocketConfigTest, InvalidDeflateConfigIsInvalid) {
  WebSocketConfig config;
  config.deflateConfig.compressionLevel = 10;
  EXPECT_THROW(config.validate(), std::invalid_argument);
}
#endif

}  // namespace aeronet::websocket
