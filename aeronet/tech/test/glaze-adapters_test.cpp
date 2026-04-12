#include "aeronet/glaze-adapters.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <glaze/glaze.hpp>
#include <string>
#include <vector>

#include "aeronet/concatenated-strings.hpp"
#include "aeronet/major-minor-version.hpp"

namespace aeronet {

namespace {

inline constexpr char kTlsPrefix[] = "TLS";
using TlsVersion = MajorMinorVersion<kTlsPrefix>;

static_assert(glz::meta<std::chrono::milliseconds>::custom_read);
static_assert(glz::meta<std::chrono::seconds>::custom_write);
static_assert(glz::meta<ConcatenatedStrings>::custom_read);
static_assert(glz::meta<ConcatenatedStrings32>::custom_read);
static_assert(glz::meta<TlsVersion>::custom_write);

template <typename StringRange>
std::vector<std::string> CollectStrings(const StringRange& values) {
  std::vector<std::string> result;
  for (auto value : values) {
    result.emplace_back(value);
  }
  return result;
}

}  // namespace

TEST(GlazeAdaptersTest, JsonSecondsRejectInvalidDurationString) {
  std::chrono::seconds value{};

  auto error = glz::read_json(value, R"("invalid")");

  EXPECT_TRUE(bool(error));
}

TEST(GlazeAdaptersTest, YamlMillisecondsRejectInvalidDurationString) {
  std::chrono::milliseconds value{};

  auto error = glz::read<glz::opts{.format = glz::YAML}>(value, "invalid");

  EXPECT_TRUE(bool(error));
}

TEST(GlazeAdaptersTest, YamlSecondsRejectInvalidDurationString) {
  std::chrono::seconds value{};

  auto error = glz::read<glz::opts{.format = glz::YAML}>(value, "invalid");

  EXPECT_TRUE(bool(error));
}

TEST(GlazeAdaptersTest, ConcatenatedStrings32JsonRoundTripNonEmpty) {
  ConcatenatedStrings32 original{"alpha", "beta"};

  auto json = glz::write_json(original);
  ASSERT_TRUE(json);

  ConcatenatedStrings32 loaded;
  auto error = glz::read_json(loaded, json.value());
  ASSERT_FALSE(bool(error));

  EXPECT_EQ(CollectStrings(loaded), (std::vector<std::string>{"alpha", "beta"}));
}

TEST(GlazeAdaptersTest, ConcatenatedStrings32YamlRoundTripNonEmpty) {
  ConcatenatedStrings32 original{"alpha", "beta"};

  auto yaml = glz::write<glz::opts{.format = glz::YAML}>(original);
  ASSERT_TRUE(yaml);

  ConcatenatedStrings32 loaded;
  auto error = glz::read<glz::opts{.format = glz::YAML}>(loaded, yaml.value());
  ASSERT_FALSE(bool(error));

  EXPECT_EQ(CollectStrings(loaded), (std::vector<std::string>{"alpha", "beta"}));
}

TEST(GlazeAdaptersTest, ConcatenatedStringsJsonRoundTripNonEmpty) {
  ConcatenatedStrings original{"gamma", "delta"};

  auto json = glz::write_json(original);
  ASSERT_TRUE(json);

  ConcatenatedStrings loaded;
  auto error = glz::read_json(loaded, json.value());
  ASSERT_FALSE(bool(error));

  EXPECT_EQ(CollectStrings(loaded), (std::vector<std::string>{"gamma", "delta"}));
}

TEST(GlazeAdaptersTest, ConcatenatedStringsYamlRoundTripNonEmpty) {
  ConcatenatedStrings original{"gamma", "delta"};

  auto yaml = glz::write<glz::opts{.format = glz::YAML}>(original);
  ASSERT_TRUE(yaml);

  ConcatenatedStrings loaded;
  auto error = glz::read<glz::opts{.format = glz::YAML}>(loaded, yaml.value());
  ASSERT_FALSE(bool(error));

  EXPECT_EQ(CollectStrings(loaded), (std::vector<std::string>{"gamma", "delta"}));
}

TEST(GlazeAdaptersTest, TlsVersionYamlAcceptsShortForm) {
  TlsVersion version;

  auto error = glz::read<glz::opts{.format = glz::YAML}>(version, R"("1.3")");
  ASSERT_FALSE(bool(error));

  EXPECT_TRUE(version.isValid());
  EXPECT_EQ(version.major(), 1);
  EXPECT_EQ(version.minor(), 3);
}

TEST(GlazeAdaptersTest, TlsVersionYamlAcceptsFullForm) {
  TlsVersion version;

  auto error = glz::read<glz::opts{.format = glz::YAML}>(version, R"("TLS1.2")");
  ASSERT_FALSE(bool(error));

  EXPECT_TRUE(version.isValid());
  EXPECT_EQ(version.major(), 1);
  EXPECT_EQ(version.minor(), 2);
}

TEST(GlazeAdaptersTest, TlsVersionYamlRejectsInvalidFullForm) {
  TlsVersion version;

  auto error = glz::read<glz::opts{.format = glz::YAML}>(version, R"("TLS1.10")");

  EXPECT_TRUE(bool(error));
}

TEST(GlazeAdaptersTest, TlsVersionYamlSerializesValidVersionAsShortForm) {
  auto yaml = glz::write<glz::opts{.format = glz::YAML}>(TlsVersion{1, 3});
  ASSERT_TRUE(yaml);

  EXPECT_TRUE(yaml.value().contains("1.3"));
}

}  // namespace aeronet