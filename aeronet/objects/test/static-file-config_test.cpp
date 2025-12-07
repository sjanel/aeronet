#include "aeronet/static-file-config.hpp"

#include <gtest/gtest.h>

#include <stdexcept>

namespace aeronet {

TEST(StaticFileConfigTest, DefaultShouldBeValid) {
  StaticFileConfig cfg;
  EXPECT_NO_THROW(cfg.validate());
}

TEST(StaticFileConfigTest, ValidateDefaultContentType) {
  StaticFileConfig cfg;
  cfg.withDefaultContentType("text/plain");
  EXPECT_NO_THROW(cfg.validate());

  cfg.withDefaultContentType("");
  EXPECT_THROW(cfg.validate(), std::invalid_argument);
}

TEST(StaticFileConfigTest, ValidateDefaultIndex) {
  StaticFileConfig cfg;
  cfg.withDefaultIndex("index.html");
  EXPECT_NO_THROW(cfg.validate());

  cfg.withDefaultIndex("invalid/index.html");
  EXPECT_THROW(cfg.validate(), std::invalid_argument);

  cfg.withDefaultIndex("another\\index.html");
  EXPECT_THROW(cfg.validate(), std::invalid_argument);
}

}  // namespace aeronet