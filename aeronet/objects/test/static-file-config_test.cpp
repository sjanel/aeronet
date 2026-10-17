#include "aeronet/static-file-config.hpp"

#include <gtest/gtest.h>

#include <stdexcept>

#ifdef AERONET_ENABLE_GLAZE
#include <glaze/glaze.hpp>
#include <glaze/yaml.hpp>
#include <string_view>

#include "aeronet/glaze-round-trip.hpp"
#endif

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

#ifdef AERONET_ENABLE_GLAZE

TEST(StaticFileConfigTest, EveryScalarFieldRoundTrips) {
  const auto roundTrip = test::MutateScalarsRoundTrip<StaticFileConfig>();
  EXPECT_FALSE(roundTrip.expected.empty());
  EXPECT_EQ(roundTrip.actual, roundTrip.expected);
}

TEST(StaticFileConfigTest, StringsRoundTrip) {
  StaticFileConfig cfg;
  cfg.withDefaultIndex("home.html").withDefaultContentType("text/plain").withDirectoryListingCss("body { margin: 0; }");
  cfg.contentTypeResolver = [](std::string_view) { return std::string_view{"text/html"}; };

  const auto json = glz::write_json(cfg).value_or("");
  // The callbacks are code, not configuration, and the packed strings are written under their own keys.
  EXPECT_FALSE(json.contains("contentTypeResolver")) << json;
  EXPECT_FALSE(json.contains("directoryIndexRenderer")) << json;
  EXPECT_FALSE(json.contains("_staticFileStrings")) << json;

  StaticFileConfig fromJson;
  ASSERT_FALSE(glz::read_json(fromJson, json));
  EXPECT_EQ(fromJson.defaultIndex(), "home.html");
  EXPECT_EQ(fromJson.defaultContentType(), "text/plain");
  EXPECT_EQ(fromJson.directoryListingCss(), "body { margin: 0; }");
  EXPECT_EQ(glz::write_json(fromJson).value_or(""), json);

  const auto yaml = glz::write<glz::opts{.format = glz::YAML}>(cfg).value_or("");
  StaticFileConfig fromYaml;
  ASSERT_FALSE(glz::read<glz::opts{.format = glz::YAML}>(fromYaml, yaml));
  EXPECT_EQ(glz::write_json(fromYaml).value_or(""), json);
}

#endif

}  // namespace aeronet
