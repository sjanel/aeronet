#include "aeronet/static-file-config.hpp"

#include <gtest/gtest.h>

#include <stdexcept>

namespace aeronet {

TEST(StaticFileConfigTest, Validate) {
  StaticFileConfig cfg;
  cfg.withDefaultIndex("index.html");
  EXPECT_NO_THROW(cfg.validate());

  cfg.withDefaultIndex("invalid/index.html");
  EXPECT_THROW(cfg.validate(), std::invalid_argument);

  cfg.withDefaultIndex("another\\index.html");
  EXPECT_THROW(cfg.validate(), std::invalid_argument);
}

}  // namespace aeronet