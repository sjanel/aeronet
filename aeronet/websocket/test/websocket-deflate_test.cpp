#include "aeronet/websocket-deflate.hpp"

#include <gtest/gtest.h>

#include <cctype>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string_view>

#include "aeronet/ndigits.hpp"
#include "aeronet/raw-chars.hpp"

#ifdef AERONET_ENABLE_ZLIB
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>

#include "aeronet/compression-test-helpers.hpp"
#include "aeronet/raw-bytes.hpp"
#endif

namespace aeronet::websocket {
namespace {

RawChars BuildDeflateResponse(const DeflateNegotiatedParams& params) {
  RawChars response(
      ComputeDeflateResponseSize(params, ndigits(params.serverMaxWindowBits), ndigits(params.clientMaxWindowBits)));

  response.setEnd(BuildDeflateResponse(params, response.data()));

  return response;
}

#ifdef AERONET_ENABLE_ZLIB
std::span<const std::byte> sv_bytes(std::string_view sv) noexcept {
  return std::as_bytes(std::span<const char>(sv.data(), sv.size()));
}

std::span<const std::byte> buf_bytes(const RawBytes& buf) noexcept { return {buf.data(), buf.size()}; }
#endif
}  // namespace

TEST(WebSocketDeflateTest, DeflateConfigValidation) {
  DeflateConfig config;

#ifdef AERONET_ENABLE_ZLIB
  // By default, config should be valid
  EXPECT_TRUE(config.enabled);
  EXPECT_NO_THROW(config.validate());

  config.serverMaxWindowBits = 7;
  EXPECT_THROW(config.validate(), std::invalid_argument);
  config.serverMaxWindowBits = 19;
  EXPECT_THROW(config.validate(), std::invalid_argument);
  config.serverMaxWindowBits = 8;
  EXPECT_NO_THROW(config.validate());
  config.serverMaxWindowBits = 15;
  EXPECT_NO_THROW(config.validate());

  config.clientMaxWindowBits = 3;
  EXPECT_THROW(config.validate(), std::invalid_argument);
  config.clientMaxWindowBits = 16;
  EXPECT_THROW(config.validate(), std::invalid_argument);
  config.clientMaxWindowBits = 8;
  EXPECT_NO_THROW(config.validate());
  config.clientMaxWindowBits = 15;
  EXPECT_NO_THROW(config.validate());

  config.compressionLevel = -1;
  EXPECT_THROW(config.validate(), std::invalid_argument);
  config.compressionLevel = 10;
  EXPECT_THROW(config.validate(), std::invalid_argument);
  config.compressionLevel = 5;
  EXPECT_NO_THROW(config.validate());

  config.minCompressSize = 0;
  EXPECT_THROW(config.validate(), std::invalid_argument);
  config.minCompressSize = 512UL;
  EXPECT_NO_THROW(config.validate());
#else
  EXPECT_FALSE(config.enabled);
  // If zlib is not enabled, validation should not throw regardless of config values
  EXPECT_NO_THROW(config.validate());
  config.enabled = true;
  EXPECT_THROW(config.validate(), std::invalid_argument);
#endif
}

// ============================================================================
// ParseDeflateOffer tests
// ============================================================================

TEST(WebSocketDeflateTest, ParseDeflateOffer_Basic) {
  DeflateConfig serverConfig;
  auto result = ParseDeflateOffer("permessage-deflate", serverConfig);

#ifdef AERONET_ENABLE_ZLIB
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value_or(DeflateNegotiatedParams{}).serverMaxWindowBits, 15);
  EXPECT_EQ(result.value_or(DeflateNegotiatedParams{}).clientMaxWindowBits, 15);
  EXPECT_FALSE(result.value_or(DeflateNegotiatedParams{}).serverNoContextTakeover);
  EXPECT_FALSE(result.value_or(DeflateNegotiatedParams{}).clientNoContextTakeover);
#else
  EXPECT_FALSE(result.has_value());
#endif
}

TEST(WebSocketDeflateTest, ParseDeflateOffer_ServerNoContextTakeover) {
  DeflateConfig serverConfig;
  auto result = ParseDeflateOffer("permessage-deflate; server_no_context_takeover", serverConfig);

#ifdef AERONET_ENABLE_ZLIB
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(result.value_or(DeflateNegotiatedParams{}).serverNoContextTakeover);
  EXPECT_FALSE(result.value_or(DeflateNegotiatedParams{}).clientNoContextTakeover);
#else
  EXPECT_FALSE(result.has_value());
#endif
}

TEST(WebSocketDeflateTest, ParseDeflateOffer_ClientNoContextTakeover) {
  DeflateConfig serverConfig;
  auto result = ParseDeflateOffer("permessage-deflate; client_no_context_takeover", serverConfig);

#ifdef AERONET_ENABLE_ZLIB
  ASSERT_TRUE(result.has_value());
  EXPECT_FALSE(result.value_or(DeflateNegotiatedParams{}).serverNoContextTakeover);
  EXPECT_TRUE(result.value_or(DeflateNegotiatedParams{}).clientNoContextTakeover);
#else
  EXPECT_FALSE(result.has_value());
#endif
}

TEST(WebSocketDeflateTest, ParseDeflateOffer_ServerMaxWindowBits) {
  DeflateConfig serverConfig;
  auto result = ParseDeflateOffer("permessage-deflate; server_max_window_bits=10", serverConfig);

#ifdef AERONET_ENABLE_ZLIB
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value_or(DeflateNegotiatedParams{}).serverMaxWindowBits, 10);
#else
  EXPECT_FALSE(result.has_value());
#endif
}

// The server compresses with 9 bits when asked for 8, which zlib does not support (see CompressDecompress_WindowBits8).
TEST(WebSocketDeflateTest, ParseDeflateOffer_WindowBits8IsAccepted) {
  DeflateConfig serverConfig;
  auto result = ParseDeflateOffer("permessage-deflate; server_max_window_bits=8", serverConfig);
#ifdef AERONET_ENABLE_ZLIB
  ASSERT_TRUE(result.has_value());
  const auto params = result.value_or(DeflateNegotiatedParams{});
  EXPECT_EQ(params.serverMaxWindowBits, 8);
  const RawChars response = BuildDeflateResponse(params);
  EXPECT_TRUE(std::string_view(response.data(), response.size()).contains("server_max_window_bits=8"));

  EXPECT_EQ(ParseDeflateOffer("permessage-deflate; client_max_window_bits=8", serverConfig)
                .value_or(DeflateNegotiatedParams{})
                .clientMaxWindowBits,
            8);
#else
  EXPECT_FALSE(result.has_value());
#endif
}

TEST(WebSocketDeflateTest, ParseDeflateOffer_ClientMaxWindowBits) {
  DeflateConfig serverConfig;
  auto result = ParseDeflateOffer("permessage-deflate; client_max_window_bits=12", serverConfig);

#ifdef AERONET_ENABLE_ZLIB
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value_or(DeflateNegotiatedParams{}).clientMaxWindowBits, 12);
#else
  EXPECT_FALSE(result.has_value());
#endif
}

TEST(WebSocketDeflateTest, ParseDeflateOffer_ClientMaxWindowBitsInvalid) {
  DeflateConfig serverConfig;
  auto result = ParseDeflateOffer("permessage-deflate; client_max_window_bits=12X", serverConfig);

  EXPECT_FALSE(result.has_value());
}

TEST(WebSocketDeflateTest, ParseDeflateOffer_InvalidWindowBitsTooSmall) {
  DeflateConfig serverConfig;
  auto result = ParseDeflateOffer("permessage-deflate; server_max_window_bits=7", serverConfig);

  // Window bits must be 8-15, so 7 is invalid
  EXPECT_FALSE(result.has_value());
}

TEST(WebSocketDeflateTest, ParseDeflateOffer_InvalidWindowBitsTooLarge) {
  DeflateConfig serverConfig;
  auto result = ParseDeflateOffer("permessage-deflate; server_max_window_bits=16", serverConfig);

  // Window bits must be 8-15, so 16 is invalid
  EXPECT_FALSE(result.has_value());
}

TEST(WebSocketDeflateTest, ParseDeflateOffer_NotPermessageDeflate) {
  DeflateConfig serverConfig;
  auto result = ParseDeflateOffer("x-webkit-deflate-frame", serverConfig);

  // Different extension, should not match
  EXPECT_FALSE(result.has_value());
}

TEST(WebSocketDeflateTest, ParseDeflateOffer_AllParameters) {
  DeflateConfig serverConfig;
  auto result = ParseDeflateOffer(
      "permessage-deflate; server_no_context_takeover; client_no_context_takeover; server_max_window_bits=9; "
      "client_max_window_bits=10",
      serverConfig);

#ifdef AERONET_ENABLE_ZLIB
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(result.value_or(DeflateNegotiatedParams{}).serverNoContextTakeover);
  EXPECT_TRUE(result.value_or(DeflateNegotiatedParams{}).clientNoContextTakeover);
  EXPECT_EQ(result.value_or(DeflateNegotiatedParams{}).serverMaxWindowBits, 9);
  EXPECT_EQ(result.value_or(DeflateNegotiatedParams{}).clientMaxWindowBits, 10);
#else
  EXPECT_FALSE(result.has_value());
#endif
}

TEST(WebSocketDeflateTest, ParseDeflateOffer_ClientMaxWindowBitsNoValue) {
  // client_max_window_bits without a value means client is advertising capability
  DeflateConfig serverConfig;
  auto result = ParseDeflateOffer("permessage-deflate; client_max_window_bits", serverConfig);

#ifdef AERONET_ENABLE_ZLIB
  ASSERT_TRUE(result.has_value());
  // Server can set the value, defaults to server's configured max
  EXPECT_EQ(result.value_or(DeflateNegotiatedParams{}).clientMaxWindowBits, 15);
#else
  EXPECT_FALSE(result.has_value());
#endif
}

TEST(WebSocketDeflateTest, ParseDeflateOffer_ServerMaxWindowBitsNoValue) {
  // server_max_window_bits without a value means server is advertising capability
  DeflateConfig serverConfig;
  auto result = ParseDeflateOffer("permessage-deflate; server_max_window_bits", serverConfig);

#ifdef AERONET_ENABLE_ZLIB
  ASSERT_TRUE(result.has_value());
  // Server can set the value, defaults to server's configured max
  EXPECT_EQ(result.value_or(DeflateNegotiatedParams{}).serverMaxWindowBits, 15);
#else
  EXPECT_FALSE(result.has_value());
#endif
}

TEST(WebSocketDeflateTest, ParseDeflateOffer_QuotedValue) {
  DeflateConfig serverConfig;
  auto result = ParseDeflateOffer("permessage-deflate; server_max_window_bits=\"10\"", serverConfig);

#ifdef AERONET_ENABLE_ZLIB
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value_or(DeflateNegotiatedParams{}).serverMaxWindowBits, 10);
#else
  EXPECT_FALSE(result.has_value());
#endif
}

TEST(WebSocketDeflateTest, ParseDeflateOffer_QuotedValueMissingEndQuote) {
  DeflateConfig serverConfig;
  auto result = ParseDeflateOffer("permessage-deflate; server_max_window_bits=\"10", serverConfig);

  // Missing end quote makes it invalid
  EXPECT_FALSE(result.has_value());
}

TEST(WebSocketDeflateTest, ParseDeflateOffer_UnknownParameterIgnored) {
  DeflateConfig serverConfig;
  auto result = ParseDeflateOffer("permessage-deflate; unknown_param=value; server_max_window_bits=10", serverConfig);

#ifdef AERONET_ENABLE_ZLIB
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value_or(DeflateNegotiatedParams{}).serverMaxWindowBits, 10);
#else
  EXPECT_FALSE(result.has_value());
#endif
}

TEST(WebSocketDeflateTest, ParseDeflateOffer_WindowBitsClampedToServerConfig) {
  DeflateConfig serverConfig;
  serverConfig.serverMaxWindowBits = 10;  // Server only supports up to 10
  auto result = ParseDeflateOffer("permessage-deflate; server_max_window_bits=15", serverConfig);

#ifdef AERONET_ENABLE_ZLIB
  ASSERT_TRUE(result.has_value());
  // Client requested 15 but server only supports 10, so clamped to 10
  EXPECT_EQ(result.value_or(DeflateNegotiatedParams{}).serverMaxWindowBits, 10);
#else
  EXPECT_FALSE(result.has_value());
#endif
}

TEST(WebSocketDeflateTest, ParseDeflateOffer_WhitespaceInParams) {
  DeflateConfig serverConfig;
  auto result = ParseDeflateOffer("permessage-deflate ;  server_max_window_bits = 10 ", serverConfig);

#ifdef AERONET_ENABLE_ZLIB
  EXPECT_EQ(result.value_or(DeflateNegotiatedParams{}).serverMaxWindowBits, 10);
#else
  EXPECT_FALSE(result.has_value());
#endif
}

TEST(WebSocketDeflateTest, ParseDeflateOffer_MaxWindowBitsNoEqualsSignShouldBeIgnored) {
  DeflateConfig serverConfig;
  auto result = ParseDeflateOffer("permessage-deflate; server_max_window_bits abc", serverConfig);

#ifdef AERONET_ENABLE_ZLIB
  ASSERT_TRUE(result.has_value());
#else
  EXPECT_FALSE(result.has_value());
#endif
}

TEST(WebSocketDeflateTest, ParseDeflateOffer_InvalidWindowBitsNonNumeric) {
  DeflateConfig serverConfig;
  auto result = ParseDeflateOffer("permessage-deflate; server_max_window_bits=abc", serverConfig);

  // Non-numeric value is invalid
  EXPECT_FALSE(result.has_value());
}

TEST(WebSocketDeflateTest, ParseDeflateOffer_InvalidWindowBits) {
  DeflateConfig serverConfig;
  auto result = ParseDeflateOffer("permessage-deflate ;  server_max_window_bits = 10X ", serverConfig);

  EXPECT_FALSE(result.has_value());
}

TEST(WebSocketDeflateTest, ParseDeflateOffer_CaseInsensitiveExtensionName) {
  DeflateConfig serverConfig;
  auto result = ParseDeflateOffer("PERMESSAGE-DEFLATE", serverConfig);

#ifdef AERONET_ENABLE_ZLIB
  ASSERT_TRUE(result.has_value());
#else
  EXPECT_FALSE(result.has_value());
#endif
}

TEST(WebSocketDeflateTest, ParseDeflateOffer_CaseInsensitiveParams) {
  DeflateConfig serverConfig;
  auto result = ParseDeflateOffer("permessage-deflate; SERVER_NO_CONTEXT_TAKEOVER", serverConfig);

#ifdef AERONET_ENABLE_ZLIB
  EXPECT_TRUE(result.value_or(DeflateNegotiatedParams{}).serverNoContextTakeover);
#else
  EXPECT_FALSE(result.has_value());
#endif
}

// ============================================================================
// BuildDeflateResponse tests
// ============================================================================

TEST(WebSocketDeflateTest, BuildDeflateResponse_Defaults) {
  DeflateNegotiatedParams params;
  RawChars response = BuildDeflateResponse(params);

  // With all defaults, should just be the extension name
  EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(response.data()), response.size()), "permessage-deflate");
}

TEST(WebSocketDeflateTest, BuildDeflateResponse_ServerNoContextTakeover) {
  DeflateNegotiatedParams params;
  params.serverNoContextTakeover = true;
  RawChars response = BuildDeflateResponse(params);

  EXPECT_TRUE(std::string_view(reinterpret_cast<const char*>(response.data()), response.size())
                  .starts_with("permessage-deflate"));
  EXPECT_TRUE(std::string_view(reinterpret_cast<const char*>(response.data()), response.size())
                  .contains("server_no_context_takeover"));
}

TEST(WebSocketDeflateTest, BuildDeflateResponse_ClientNoContextTakeover) {
  DeflateNegotiatedParams params;
  params.clientNoContextTakeover = true;
  RawChars response = BuildDeflateResponse(params);

  EXPECT_TRUE(std::string_view(reinterpret_cast<const char*>(response.data()), response.size())
                  .contains("client_no_context_takeover"));
}

TEST(WebSocketDeflateTest, BuildDeflateResponse_ReducedWindowBits) {
  DeflateNegotiatedParams params;
  params.serverMaxWindowBits = 10;
  params.clientMaxWindowBits = 12;
  RawChars response = BuildDeflateResponse(params);

  EXPECT_TRUE(std::string_view(reinterpret_cast<const char*>(response.data()), response.size())
                  .contains("server_max_window_bits=10"));
  EXPECT_TRUE(std::string_view(reinterpret_cast<const char*>(response.data()), response.size())
                  .contains("client_max_window_bits=12"));
}

TEST(WebSocketDeflateTest, BuildDeflateResponse_AllParams) {
  DeflateNegotiatedParams params;
  params.serverNoContextTakeover = true;
  params.clientNoContextTakeover = true;
  params.serverMaxWindowBits = 9;
  params.clientMaxWindowBits = 10;
  RawChars response = BuildDeflateResponse(params);

  EXPECT_TRUE(std::string_view(reinterpret_cast<const char*>(response.data()), response.size())
                  .contains("server_no_context_takeover"));
  EXPECT_TRUE(std::string_view(reinterpret_cast<const char*>(response.data()), response.size())
                  .contains("client_no_context_takeover"));
  EXPECT_TRUE(std::string_view(reinterpret_cast<const char*>(response.data()), response.size())
                  .contains("server_max_window_bits=9"));
  EXPECT_TRUE(std::string_view(reinterpret_cast<const char*>(response.data()), response.size())
                  .contains("client_max_window_bits=10"));
}

TEST(WebSocketDeflateTest, BuildDeflateResponse_DefaultWindowBitsNotIncluded) {
  DeflateNegotiatedParams params;
  params.serverMaxWindowBits = 15;  // Default
  params.clientMaxWindowBits = 15;  // Default
  RawChars response = BuildDeflateResponse(params);

  // Default window bits should not be included
  EXPECT_TRUE(
      !std::string_view(reinterpret_cast<const char*>(response.data()), response.size()).contains("window_bits"));
}

TEST(WebSocketDeflateTest, ParseDeflateOffer_ClientWindowIsOnlyLimitedWhenOffered) {
  DeflateConfig serverConfig;
  serverConfig.clientMaxWindowBits = 10;
  // A client not offering client_max_window_bits compresses with a 32 KiB window: the response must not limit it (RFC
  // 7692 section 7.1.2.2).
  for (std::string_view offer : {"permessage-deflate", "permessage-deflate; server_no_context_takeover"}) {
    auto result = ParseDeflateOffer(offer, serverConfig);
#ifdef AERONET_ENABLE_ZLIB
    ASSERT_TRUE(result.has_value()) << offer;
    const auto params = result.value_or(DeflateNegotiatedParams{});
    EXPECT_EQ(params.clientMaxWindowBits, 15) << offer;
    const RawChars response = BuildDeflateResponse(params);
    EXPECT_FALSE(std::string_view(response.data(), response.size()).contains("client_max_window_bits")) << offer;
#else
    EXPECT_FALSE(result.has_value());
#endif
  }

#ifdef AERONET_ENABLE_ZLIB
  // Offered: limited to the configured window, or to the offered one if smaller.
  EXPECT_EQ(ParseDeflateOffer("permessage-deflate; client_max_window_bits", serverConfig)
                .value_or(DeflateNegotiatedParams{})
                .clientMaxWindowBits,
            10);
  EXPECT_EQ(ParseDeflateOffer("permessage-deflate; client_max_window_bits=12", serverConfig)
                .value_or(DeflateNegotiatedParams{})
                .clientMaxWindowBits,
            10);
  EXPECT_EQ(ParseDeflateOffer("permessage-deflate; client_max_window_bits=9", serverConfig)
                .value_or(DeflateNegotiatedParams{})
                .clientMaxWindowBits,
            9);
#endif
}

// ============================================================================
// DeflateContext compress/decompress tests
// ============================================================================

#ifdef AERONET_ENABLE_ZLIB
TEST(WebSocketDeflateTest, CompressDecompress_RoundTrip) {
  DeflateNegotiatedParams params;
  DeflateConfig config;
  DeflateContext ctx(params, config, true);

  std::string_view original = "Hello, WebSocket world! This is a test message that should be compressed.";
  auto inputSpan = sv_bytes(original);

  // Compress
  RawBytes compressed;
  ASSERT_EQ(ctx.compress(inputSpan, compressed), nullptr);
  EXPECT_GT(compressed.size(), 0UL);

  // Decompress
  RawBytes decompressed;
  auto compressedSpan = buf_bytes(compressed);
  ASSERT_EQ(ctx.decompress(compressedSpan, decompressed), nullptr);

  // Verify round-trip
  std::string_view result(reinterpret_cast<const char*>(decompressed.data()), decompressed.size());
  EXPECT_EQ(result, original);
}

TEST(WebSocketDeflateTest, CompressDecompress_LargeData) {
  DeflateNegotiatedParams params;
  DeflateConfig config;
  DeflateContext ctx(params, config, true);

  // Create a large repetitive string (compresses well)
  auto original = test::MakePatternedPayload(10000UL);

  auto inputSpan = sv_bytes(original);

  // Compress
  RawBytes compressed;
  ASSERT_EQ(ctx.compress(inputSpan, compressed), nullptr);

  // Should achieve compression
  EXPECT_LT(compressed.size(), original.size());

  // Decompress
  RawBytes decompressed;
  auto compressedSpan = buf_bytes(compressed);
  ASSERT_EQ(ctx.decompress(compressedSpan, decompressed), nullptr);

  EXPECT_EQ(decompressed.size(), original.size());
  EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(decompressed.data()), decompressed.size()), original);
}

TEST(WebSocketDeflateTest, CompressDecompress_EmptyData) {
  DeflateNegotiatedParams params;
  DeflateConfig config;
  DeflateContext ctx(params, config, true);

  auto inputSpan = sv_bytes(std::string_view());

  RawBytes compressed;
  EXPECT_EQ(ctx.compress(inputSpan, compressed), nullptr);

  RawBytes decompressed;
  auto compressedSpan = buf_bytes(compressed);
  EXPECT_EQ(ctx.decompress(compressedSpan, decompressed), nullptr);
  EXPECT_TRUE(decompressed.empty());
}

TEST(WebSocketDeflateTest, ShouldSkipCompression_BelowThreshold) {
  DeflateNegotiatedParams params;
  DeflateConfig config;
  config.minCompressSize = 100;
  DeflateContext ctx(params, config, true);

  EXPECT_TRUE(ctx.shouldSkipCompression(50));
  EXPECT_TRUE(ctx.shouldSkipCompression(99));
  EXPECT_FALSE(ctx.shouldSkipCompression(100));
  EXPECT_FALSE(ctx.shouldSkipCompression(200));
}

TEST(WebSocketDeflateTest, DecompressSizeLimit) {
  DeflateNegotiatedParams params;
  DeflateConfig config;
  DeflateContext ctx(params, config, true);

  // Create a large string that compresses well
  auto original = test::MakePatternedPayload(10000UL);
  auto inputSpan = sv_bytes(original);

  // Compress
  RawBytes compressed;
  ASSERT_EQ(ctx.compress(inputSpan, compressed), nullptr);

  // Try to decompress with a small limit
  RawBytes decompressed;
  auto compressedSpan = buf_bytes(compressed);
  EXPECT_STREQ(ctx.decompress(compressedSpan, decompressed, 100),
               "Decompressed size exceeds maximum");  // 100 byte limit
}

// Additional coverage tests
TEST(WebSocketDeflateTest, CompressDecompress_ClientSide) {
  DeflateNegotiatedParams params;
  DeflateConfig config;
  // Create client-side context
  DeflateContext ctx(params, config, false);

  std::string_view original = "Client-side compression test message.";
  auto inputSpan = sv_bytes(original);

  RawBytes compressed;
  ASSERT_EQ(ctx.compress(inputSpan, compressed), nullptr);

  RawBytes decompressed;
  auto compressedSpan = buf_bytes(compressed);
  ASSERT_EQ(ctx.decompress(compressedSpan, decompressed), nullptr);

  EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(decompressed.data()), decompressed.size()), original);
}

TEST(WebSocketDeflateTest, CompressDecompress_WithNoContextTakeover) {
  DeflateNegotiatedParams params;
  params.serverNoContextTakeover = true;
  params.clientNoContextTakeover = true;
  DeflateConfig config;
  DeflateContext ctx(params, config, true);

  // Compress multiple messages - each should work independently due to no_context_takeover
  std::string_view msg1 = "First message for testing.";
  std::string_view msg2 = "Second message for testing.";

  auto input1 = sv_bytes(msg1);
  auto input2 = sv_bytes(msg2);

  RawBytes compressed1;
  RawBytes compressed2;
  ASSERT_EQ(ctx.compress(input1, compressed1), nullptr);
  ASSERT_EQ(ctx.compress(input2, compressed2), nullptr);

  RawBytes decompressed1;
  RawBytes decompressed2;
  ASSERT_EQ(ctx.decompress(buf_bytes(compressed1), decompressed1), nullptr);
  ASSERT_EQ(ctx.decompress(buf_bytes(compressed2), decompressed2), nullptr);

  EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(decompressed1.data()), decompressed1.size()), msg1);
  EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(decompressed2.data()), decompressed2.size()), msg2);
}

TEST(WebSocketDeflateTest, CompressDecompress_ReducedWindowBits) {
  DeflateNegotiatedParams params;
  params.serverMaxWindowBits = 9;
  params.clientMaxWindowBits = 9;
  DeflateConfig config;
  DeflateContext ctx(params, config, true);

  std::string_view original = "Test with reduced window bits setting.";
  auto inputSpan = sv_bytes(original);

  RawBytes compressed;
  ASSERT_EQ(ctx.compress(inputSpan, compressed), nullptr);

  RawBytes decompressed;
  auto compressedSpan = buf_bytes(compressed);
  ASSERT_EQ(ctx.decompress(compressedSpan, decompressed), nullptr);

  EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(decompressed.data()), decompressed.size()), original);
}

// zlib cannot compress with the 256 bytes window that may be negotiated for either side: the sender compresses with 512
// bytes, whose matches never reach further than 250 bytes, valid for the window of the receiver.
TEST(WebSocketDeflateTest, CompressDecompress_WindowBits8) {
  DeflateNegotiatedParams params;
  params.serverMaxWindowBits = 8;
  params.clientMaxWindowBits = 8;
  DeflateConfig config;
  DeflateContext client(params, config, false);
  DeflateContext server(params, config, true);

  // Pseudo-random bytes with repeats at distances from 240 to 511 bytes, which a 512 bytes window could reach.
  std::string original(16UL * 1024UL, '\0');
  uint32_t state = 12345;
  const auto next = [&state] {
    state = (state * 1103515245U) + 12345U;
    return state >> 16U;
  };
  for (char& ch : original) {
    ch = static_cast<char>(next());
  }
  for (std::size_t pos = 512; pos + 32 <= original.size(); pos += 64) {
    const std::size_t distance = 240 + (next() % 272);
    for (std::size_t idx = pos; idx < pos + 32; ++idx) {
      original[idx] = original[idx - distance];
    }
  }

  // Several messages each way: matches may also reach back into the previous ones (context takeover).
  for (int msg = 0; msg < 3; ++msg) {
    for (auto [sender, receiver] : {std::pair{&client, &server}, std::pair{&server, &client}}) {
      RawBytes compressed;
      ASSERT_EQ(sender->compress(sv_bytes(original), compressed), nullptr);
      RawBytes decompressed;
      ASSERT_EQ(receiver->decompress(buf_bytes(compressed), decompressed), nullptr);
      EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(decompressed.data()), decompressed.size()), original);
    }
  }
}

TEST(WebSocketDeflateTest, CompressDecompress_DifferentCompressionLevel) {
  DeflateNegotiatedParams params;
  DeflateConfig config;
  config.compressionLevel = 9;  // Maximum compression
  DeflateContext ctx(params, config, true);

  std::string_view original = "Test with high compression level for maximum ratio.";
  auto inputSpan = sv_bytes(original);

  RawBytes compressed;
  ASSERT_EQ(ctx.compress(inputSpan, compressed), nullptr);

  RawBytes decompressed;
  auto compressedSpan = buf_bytes(compressed);
  ASSERT_EQ(ctx.decompress(compressedSpan, decompressed), nullptr);

  EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(decompressed.data()), decompressed.size()), original);
}

TEST(WebSocketDeflateTest, ShouldSkipCompression_ZeroThreshold) {
  DeflateNegotiatedParams params;
  DeflateConfig config;
  config.minCompressSize = 0;  // Always compress
  DeflateContext ctx(params, config, true);

  EXPECT_FALSE(ctx.shouldSkipCompression(0));
  EXPECT_FALSE(ctx.shouldSkipCompression(1));
  EXPECT_FALSE(ctx.shouldSkipCompression(1000));
}

TEST(WebSocketDeflateTest, DecompressZeroSizeLimit) {
  DeflateNegotiatedParams params;
  DeflateConfig config;
  DeflateContext ctx(params, config, true);

  std::string_view original = "Test message for unlimited decompression.";
  auto inputSpan = sv_bytes(original);

  RawBytes compressed;
  ASSERT_EQ(ctx.compress(inputSpan, compressed), nullptr);

  // Zero limit means unlimited
  RawBytes decompressed;
  auto compressedSpan = buf_bytes(compressed);
  EXPECT_EQ(ctx.decompress(compressedSpan, decompressed, 0), nullptr);
  EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(decompressed.data()), decompressed.size()), original);
}

#endif  // AERONET_ENABLE_ZLIB

}  // namespace aeronet::websocket
