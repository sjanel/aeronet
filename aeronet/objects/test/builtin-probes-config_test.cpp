#include "aeronet/builtin-probes-config.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

namespace aeronet {

TEST(BuiltinProbesConfigTest, Default) {
  BuiltinProbesConfig config;
  EXPECT_NO_THROW(config.validate());
}

TEST(BuiltinProbesConfigTest, ValidPaths) {
  BuiltinProbesConfig config;
  config.enabled = true;
  config.withLivenessPath("/somepath");
  config.withReadinessPath("/some-other-path");
  config.withStartupPath("/start");

  EXPECT_NO_THROW(config.validate());
}

TEST(BuiltinProbesConfigTest, EmptyPath) {
  BuiltinProbesConfig config;
  config.enabled = true;
  config.withLivenessPath("");
  EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(BuiltinProbesConfigTest, PathWithoutLeadingSlash) {
  BuiltinProbesConfig config;
  config.enabled = true;
  config.withReadinessPath("noleadingslash");
  EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(BuiltinProbesConfigTest, PathWithInvalidCharacters) {
  BuiltinProbesConfig config;
  config.enabled = true;
  config.withStartupPath("/validpath/with space");
  EXPECT_THROW(config.validate(), std::invalid_argument);

  config.withStartupPath("/validpath/with\x01controlchar");
  EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(BuiltinProbesConfigTest, DisableValidation) {
  BuiltinProbesConfig config;
  config.enabled = false;
  config.withLivenessPath("");
  config.withReadinessPath("noleadingslash");
  config.withStartupPath("/validpath/with space");

  EXPECT_NO_THROW(config.validate());
}

TEST(BuiltinProbesConfigTest, ControlCharacterInvalid) {
  BuiltinProbesConfig config;
  config.enabled = true;
  std::string somePath("/validpath/with");
  somePath.push_back('\x7F');  // DEL control character
  somePath.append("delchar");
  config.withLivenessPath(somePath);
  EXPECT_THROW(config.validate(), std::invalid_argument);
}

}  // namespace aeronet