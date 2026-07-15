#include "aeronet/http-client-config.hpp"

#include <gtest/gtest.h>

#include <stdexcept>

#include "aeronet/http-constants.hpp"
#include "aeronet/http-header.hpp"
#include "aeronet/vector.hpp"

namespace aeronet {

TEST(HttpClientConfigTest, WithGlobalHeadersShouldReplaceAllList) {
  HttpClientConfig config;
  config.withGlobalHeaders(vector<http::Header>{http::Header{"X-Valid", "value"}, http::Header{"X-Custom", "value"}});
  EXPECT_EQ(config.globalHeaders.nbConcatenatedStrings(), 2U);
  config.withGlobalHeaders(vector<http::Header>{http::Header{"X-Valid2", "value"}, http::Header{"X-Custom2", "value"}});
  EXPECT_EQ(config.globalHeaders.nbConcatenatedStrings(), 2U);
  config.withGlobalHeaders({});
  EXPECT_TRUE(config.globalHeaders.empty());

  EXPECT_NO_THROW(config.validate());
}

TEST(HttpClientConfigTest, InvalidGlobalHeaderValueWithControlChars) {
  HttpClientConfig config;
  config.globalHeaders.append("X-Test:value\x01");  // control char 0x01
  EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(HttpClientConfigTest, NoGlobalHeadersIsValidAndShouldNotAddDefaultOnes) {
  HttpClientConfig config;
  config.withGlobalHeaders({});
  EXPECT_TRUE(config.globalHeaders.empty());
  EXPECT_NO_THROW(config.validate());
  EXPECT_TRUE(config.globalHeaders.empty());
}

TEST(HttpClientConfigTest, AddGlobalHeader) {
  HttpClientConfig config;
  config.addGlobalHeader(http::Header{"X-Test", "value"});
  EXPECT_NO_THROW(config.validate());
}

TEST(HttpClientConfigTest, HeaderKey1) {
  HttpClientConfig config;
  config.withGlobalHeaders(vector<http::Header>{http::Header{"X-Valid", "value"}, http::Header{"X-Custom", "value"}});

  EXPECT_NO_THROW(config.validate());
}

TEST(HttpClientConfigTest, HeaderKey2) {
  HttpClientConfig config;
  config.globalHeaders.append(":value");

  EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(HttpClientConfigTest, HeaderKey3) {
  HttpClientConfig config;
  config.globalHeaders.append("Invalid Char!: value");
  EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(HttpClientConfigTest, HeaderKey4) {
  HttpClientConfig config;
  config.globalHeaders.append("Another@Invalid: value");  // invalid char '@'
  EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(HttpClientConfigTest, ReservedGlobalHeaderShouldThrow) {
  HttpClientConfig config;
  config.globalHeaders.append("Content-Length: 10");
  EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(HttpClientConfigTest, InvalidGlobalHeaderShouldThrow1) {
  HttpClientConfig config;
  config.globalHeaders.append("Invalid\nHeader: value");
  EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(HttpClientConfigTest, InvalidGlobalHeaderShouldThrow2) {
  HttpClientConfig config;
  config.globalHeaders.append("X-Custom: value\x7F");  // DEL control char
  EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(HttpClientConfigTest, InvalidGlobalHeaderShouldThrow3) {
  HttpClientConfig config;
  config.globalHeaders.append("X-Cust:om: value");
  EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(HttpClientConfigTest, GlobalHeaderShouldContainHeaderSep1) {
  HttpClientConfig config;
  config.globalHeaders.append("InvalidNoColon");
  EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(HttpClientConfigTest, GlobalHeaderShouldContainHeaderSep2) {
  HttpClientConfig config;
  config.globalHeaders.append("X-Custom:value");
  ASSERT_FALSE(config.globalHeaders.contains(http::HeaderSep));
  EXPECT_THROW(config.validate(), std::invalid_argument);
}

}  // namespace aeronet