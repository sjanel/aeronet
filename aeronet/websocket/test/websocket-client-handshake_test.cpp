#include "aeronet/websocket-client-handshake.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "aeronet/websocket-config.hpp"
#include "aeronet/websocket-upgrade.hpp"

namespace aeronet::websocket {

namespace {

// The key and accept values of RFC 6455 §1.3.
constexpr std::string_view kKey = "dGhlIHNhbXBsZSBub25jZQ==";
constexpr std::string_view kAccept = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=";

UpgradeResponseHeaders ValidHeaders() {
  return {.upgrade = "websocket", .connection = "Upgrade", .accept = kAccept, .protocol = {}, .extensions = {}};
}

WebSocketConfig NoDeflateConfig() {
  WebSocketConfig config;
#ifdef AERONET_ENABLE_ZLIB
  config.deflateConfig.enabled = false;
#endif
  return config;
}

}  // namespace

TEST(WebSocketClientHandshake, GeneratesValidDistinctKeys) {
  const WebSocketKey key1 = GenerateWebSocketKey();
  const WebSocketKey key2 = GenerateWebSocketKey();
  EXPECT_TRUE(IsValidWebSocketKey(std::string_view(key1.data(), key1.size())));
  EXPECT_TRUE(IsValidWebSocketKey(std::string_view(key2.data(), key2.size())));
  EXPECT_NE(key1, key2);
}

TEST(WebSocketClientHandshake, AcceptsAValidResponse) {
  const UpgradeResponseValidation validation = ValidateUpgradeResponse(ValidHeaders(), kKey, {}, NoDeflateConfig());
  EXPECT_TRUE(validation.valid());
#ifdef AERONET_ENABLE_ZLIB
  EXPECT_FALSE(validation.deflateParams.hasValue());
#endif
  // Case-insensitive tokens, list of Connection options, surrounding whitespace, empty extension elements.
  UpgradeResponseHeaders headers = ValidHeaders();
  headers.upgrade = " WebSocket ";
  headers.connection = "keep-alive, upgrade";
  headers.accept = " s3pPLMBiTxaQ9kYGzzhZRbK+xOo= ";
  headers.extensions = " , ";
  EXPECT_TRUE(ValidateUpgradeResponse(headers, kKey, {}, NoDeflateConfig()).valid());
}

namespace {

struct InvalidResponseCase {
  std::string_view name;
  UpgradeResponseHeaders headers;
  std::string_view offeredProtocols;
  std::string_view expectedError;
};

}  // namespace

TEST(WebSocketClientHandshake, RejectsInvalidResponses) {
  static constexpr std::string_view kBadUpgrade = "Upgrade header of the response is not 'websocket'";
  static constexpr std::string_view kBadConnection = "Connection header of the response does not contain 'upgrade'";
  static constexpr std::string_view kBadAccept =
      "Sec-WebSocket-Accept of the response does not match the key of the request";
  static constexpr std::string_view kBadProtocol =
      "Sec-WebSocket-Protocol of the response is not one of the offered subprotocols";
  static constexpr std::string_view kBadExtension =
      "Sec-WebSocket-Extensions of the response holds an extension that was not offered";
  const UpgradeResponseHeaders valid = ValidHeaders();
  const auto with = [&valid](auto modifier) {
    UpgradeResponseHeaders headers = valid;
    modifier(headers);
    return headers;
  };
  const InvalidResponseCase cases[]{
      {"missing upgrade", with([](UpgradeResponseHeaders& hdrs) { hdrs.upgrade = {}; }), {}, kBadUpgrade},
      {"upgrade list", with([](UpgradeResponseHeaders& hdrs) { hdrs.upgrade = "websocket, h2c"; }), {}, kBadUpgrade},
      {"missing connection", with([](UpgradeResponseHeaders& hdrs) { hdrs.connection = {}; }), {}, kBadConnection},
      {"connection close", with([](UpgradeResponseHeaders& hdrs) { hdrs.connection = "close"; }), {}, kBadConnection},
      {"missing accept", with([](UpgradeResponseHeaders& hdrs) { hdrs.accept = {}; }), {}, kBadAccept},
      {
          "other accept",
          with([](UpgradeResponseHeaders& hdrs) { hdrs.accept = "AAAAAAAAAAAAAAAAAAAAAAAAAAA="; }),
          {},
          kBadAccept,
      },
      {"protocol without offer", with([](UpgradeResponseHeaders& hdrs) { hdrs.protocol = "chat"; }), {}, kBadProtocol},
      {
          "protocol not offered",
          with([](UpgradeResponseHeaders& hdrs) { hdrs.protocol = "chat"; }),
          "v1, v2",
          kBadProtocol,
      },
      {
          "protocol of another case",
          with([](UpgradeResponseHeaders& hdrs) { hdrs.protocol = "V2"; }),
          "v1, v2",
          kBadProtocol,
      },
      {
          "several protocols",
          with([](UpgradeResponseHeaders& hdrs) { hdrs.protocol = "v1, v2"; }),
          "v1, v2",
          kBadProtocol,
      },
      {
          "extension not offered",
          with([](UpgradeResponseHeaders& hdrs) { hdrs.extensions = "x-webkit"; }),
          {},
          kBadExtension,
      },
      {
          "deflate not offered",
          with([](UpgradeResponseHeaders& hdrs) { hdrs.extensions = "permessage-deflate"; }),
          {},
          kBadExtension,
      },
  };
  for (const InvalidResponseCase& testCase : cases) {
    const UpgradeResponseValidation validation =
        ValidateUpgradeResponse(testCase.headers, kKey, testCase.offeredProtocols, NoDeflateConfig());
    ASSERT_FALSE(validation.valid()) << testCase.name;
    EXPECT_EQ(std::string_view(validation.pErrorMessage), testCase.expectedError) << testCase.name;
  }
}

TEST(WebSocketClientHandshake, AcceptsAnOfferedSubprotocol) {
  UpgradeResponseHeaders headers = ValidHeaders();
  headers.protocol = " v2 ";
  EXPECT_TRUE(ValidateUpgradeResponse(headers, kKey, "v1, v2", NoDeflateConfig()).valid());
  EXPECT_TRUE(ValidateUpgradeResponse(headers, kKey, "v2", NoDeflateConfig()).valid());
}

#ifdef AERONET_ENABLE_ZLIB
TEST(WebSocketClientHandshake, NegotiatesTheDeflateOffer) {
  const WebSocketConfig config;  // deflate offered by default
  UpgradeResponseHeaders headers = ValidHeaders();
  headers.extensions = "permessage-deflate; server_max_window_bits=10; client_max_window_bits=12";
  const UpgradeResponseValidation validation = ValidateUpgradeResponse(headers, kKey, {}, config);
  ASSERT_TRUE(validation.valid());
  EXPECT_EQ(validation.deflateParams.serverMaxWindowBits, 10);
  EXPECT_EQ(validation.deflateParams.clientMaxWindowBits, 12);

  // Accepted once: a second element, an invalid one, or another extension, fails the connection.
  static constexpr std::string_view kNotOffered =
      "Sec-WebSocket-Extensions of the response holds an extension that was not offered";
  static constexpr std::string_view kInvalidParameters =
      "Sec-WebSocket-Extensions of the response holds invalid permessage-deflate parameters";
  struct Case {
    std::string_view extensions;
    std::string_view expectedError;
  };
  for (const Case& testCase : {
           Case{"permessage-deflate, permessage-deflate", kNotOffered},
           Case{"permessage-deflate; unknown", kInvalidParameters},
           Case{"Permessage-Deflate; server_max_window_bits=16", kInvalidParameters},
           Case{
               "permessage-deflate, x-other",
               kNotOffered,
           },
       }) {
    headers.extensions = testCase.extensions;
    const UpgradeResponseValidation invalid = ValidateUpgradeResponse(headers, kKey, {}, config);
    ASSERT_FALSE(invalid.valid()) << testCase.extensions;
    EXPECT_EQ(std::string_view(invalid.pErrorMessage), testCase.expectedError) << testCase.extensions;
  }
}
#endif

}  // namespace aeronet::websocket
