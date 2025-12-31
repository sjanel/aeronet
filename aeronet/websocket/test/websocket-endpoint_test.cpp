#include "aeronet/websocket-endpoint.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "aeronet/connection-state.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/websocket-handler.hpp"

namespace aeronet {
namespace {

// ============================================================================
// WebSocketEndpoint construction tests
// ============================================================================

TEST(WebSocketEndpointTest, DefaultConstruction) {
  WebSocketEndpoint endpoint;

  EXPECT_TRUE(endpoint.supportedProtocols.empty());
  EXPECT_FALSE(endpoint.factory);  // No factory set by default
}

// ============================================================================
// WithFactory tests
// ============================================================================

TEST(WebSocketEndpointTest, WithFactory_CreatesEndpointWithFactory) {
  auto endpoint = WebSocketEndpoint::WithFactory([](const HttpRequest& /*request*/) {
    return std::make_unique<websocket::WebSocketHandler>(websocket::WebSocketConfig{});
  });

  EXPECT_TRUE(static_cast<bool>(endpoint.factory));
}

// ============================================================================
// WithCallbacks tests
// ============================================================================

TEST(WebSocketEndpointTest, WithCallbacks_CreatesEndpointWithFactory) {
  websocket::WebSocketCallbacks callbacks;
  callbacks.onMessage = [](std::span<const std::byte> /*payload*/, bool /*isBinary*/) {};

  auto endpoint = WebSocketEndpoint::WithCallbacks(std::move(callbacks));

  EXPECT_TRUE(static_cast<bool>(endpoint.factory));
}

TEST(WebSocketEndpointTest, WithCallbacks_FactoryCreatesHandler) {
  websocket::WebSocketCallbacks callbacks;
  callbacks.onMessage = [](std::span<const std::byte> /*payload*/, bool /*isBinary*/) {};

  auto endpoint = WebSocketEndpoint::WithCallbacks(std::move(callbacks));
  ConnectionState cs;
  auto handler = endpoint.factory(cs.request);
  ASSERT_NE(handler, nullptr);
  EXPECT_TRUE(handler->config().isServerSide);
}

// ============================================================================
// WithConfigAndCallbacks tests
// ============================================================================

TEST(WebSocketEndpointTest, WithConfigAndCallbacks_SetsConfig) {
  websocket::WebSocketConfig config;
  config.isServerSide = true;
  config.maxMessageSize = 12345;

  websocket::WebSocketCallbacks callbacks;
  callbacks.onMessage = [](std::span<const std::byte> /*payload*/, bool /*isBinary*/) {};

  auto endpoint = WebSocketEndpoint::WithConfigAndCallbacks(config, std::move(callbacks));

  EXPECT_TRUE(static_cast<bool>(endpoint.factory));
  EXPECT_TRUE(endpoint.config.isServerSide);
  EXPECT_EQ(endpoint.config.maxMessageSize, 12345);
}

// ============================================================================
// WithProtocolsAndCallbacks tests
// ============================================================================

TEST(WebSocketEndpointTest, WithProtocolsAndCallbacks_SetsProtocols) {
  std::array<std::string, 2> protocols = {"graphql-ws", "chat"};
  websocket::WebSocketCallbacks callbacks;

  auto endpoint = WebSocketEndpoint::WithProtocolsAndCallbacks(protocols, std::move(callbacks));

  EXPECT_TRUE(static_cast<bool>(endpoint.factory));
  EXPECT_EQ(endpoint.supportedProtocols.nbConcatenatedStrings(), 2);
  EXPECT_TRUE(endpoint.supportedProtocols.contains("graphql-ws"));
  EXPECT_TRUE(endpoint.supportedProtocols.contains("chat"));
}

TEST(WebSocketEndpointTest, WithProtocolsAndCallbacks_FactoryCreatesHandler) {
  std::array<std::string, 2> protocols = {"graphql-ws", "chat"};
  websocket::WebSocketCallbacks callbacks;

  auto endpoint = WebSocketEndpoint::WithProtocolsAndCallbacks(protocols, std::move(callbacks));
  ConnectionState cs;
  auto handler = endpoint.factory(cs.request);
  ASSERT_NE(handler, nullptr);
  EXPECT_TRUE(handler->config().isServerSide);
}

TEST(WebSocketEndpointTest, WithProtocolsAndCallbacks_EmptyProtocols) {
  std::span<const std::string> emptyProtocols;
  websocket::WebSocketCallbacks callbacks;

  auto endpoint = WebSocketEndpoint::WithProtocolsAndCallbacks(emptyProtocols, std::move(callbacks));

  EXPECT_TRUE(static_cast<bool>(endpoint.factory));
  EXPECT_TRUE(endpoint.supportedProtocols.empty());
}

// ============================================================================
// WithFullConfig tests
// ============================================================================

TEST(WebSocketEndpointTest, WithFullConfig_SetsAllFields) {
  websocket::WebSocketConfig config;
  config.isServerSide = true;
  config.maxMessageSize = 5000;
  config.maxFrameSize = 1000;

  std::array<std::string, 3> protocols = {"proto1", "proto2", "proto3"};

  websocket::WebSocketCallbacks callbacks;
  callbacks.onPing = [](std::span<const std::byte> /*payload*/) {};

  auto endpoint = WebSocketEndpoint::WithFullConfig(config, protocols, std::move(callbacks));

  // Verify config
  EXPECT_TRUE(endpoint.config.isServerSide);
  EXPECT_EQ(endpoint.config.maxMessageSize, 5000);
  EXPECT_EQ(endpoint.config.maxFrameSize, 1000);

  // Verify protocols
  EXPECT_EQ(endpoint.supportedProtocols.nbConcatenatedStrings(), 3);
  EXPECT_TRUE(endpoint.supportedProtocols.contains("proto1"));
  EXPECT_TRUE(endpoint.supportedProtocols.contains("proto2"));
  EXPECT_TRUE(endpoint.supportedProtocols.contains("proto3"));

  // Verify factory is set
  EXPECT_TRUE(static_cast<bool>(endpoint.factory));
}

TEST(WebSocketEndpointTest, WithFullConfig_FactoryCreatesHandler) {
  websocket::WebSocketConfig config;
  config.isServerSide = false;  // test non-default
  config.maxMessageSize = 7777;

  std::array<std::string, 2> protocols = {"protoA", "protoB"};
  websocket::WebSocketCallbacks callbacks;

  auto endpoint = WebSocketEndpoint::WithFullConfig(config, protocols, std::move(callbacks));
  ConnectionState cs;
  auto handler = endpoint.factory(cs.request);
  ASSERT_NE(handler, nullptr);
  EXPECT_FALSE(handler->config().isServerSide);
  EXPECT_EQ(handler->config().maxMessageSize, 7777);
}

TEST(WebSocketEndpointTest, WithFullConfig_EmptyProtocols) {
  websocket::WebSocketConfig config;
  std::span<const std::string> emptyProtocols;
  websocket::WebSocketCallbacks callbacks;

  auto endpoint = WebSocketEndpoint::WithFullConfig(config, emptyProtocols, std::move(callbacks));

  EXPECT_TRUE(static_cast<bool>(endpoint.factory));
  EXPECT_TRUE(endpoint.supportedProtocols.empty());
}

// ============================================================================
// Combined usage tests
// ============================================================================

TEST(WebSocketEndpointTest, CombinedUsage_WithCompressionEnabled) {
  websocket::WebSocketConfig config;

  config.deflateConfig.enabled = true;
  config.isServerSide = true;

  std::array<std::string, 1> protocols = {"graphql-ws"};

  auto endpoint = WebSocketEndpoint::WithFullConfig(config, protocols, {});

  EXPECT_TRUE(endpoint.config.deflateConfig.enabled);
  EXPECT_EQ(endpoint.supportedProtocols.nbConcatenatedStrings(), 1);
  EXPECT_TRUE(static_cast<bool>(endpoint.factory));
}

}  // namespace
}  // namespace aeronet
