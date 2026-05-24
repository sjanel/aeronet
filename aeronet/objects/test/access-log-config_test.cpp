#include "aeronet/access-log-config.hpp"

#include <gtest/gtest.h>

#include <stdexcept>

using namespace aeronet;

TEST(AccessLogConfigTest, ValidateDoesNotThrowWhenSinkIsNone) {
  AccessLogConfig cfg;
  cfg.sink = AccessLogConfig::Sink::None;

  EXPECT_NO_THROW(cfg.validate());
}

TEST(AccessLogConfigTest, ValidateThrowsWhenFileSinkHasEmptyPath) {
  AccessLogConfig cfg;
  cfg.sink = AccessLogConfig::Sink::File;
  cfg.filePath.clear();

  EXPECT_THROW(cfg.validate(), std::invalid_argument);
}

TEST(AccessLogConfigTest, ValidateDoesNotThrowWhenFileSinkHasPath) {
  AccessLogConfig cfg;
  cfg.sink = AccessLogConfig::Sink::File;
  cfg.filePath = "access.log";

  EXPECT_NO_THROW(cfg.validate());
}
