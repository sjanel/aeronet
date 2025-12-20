#include "aeronet/http2-protocol-handler.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http2-config.hpp"
#include "aeronet/protocol-handler.hpp"
#include "aeronet/router.hpp"

namespace aeronet::http2 {
namespace {

// ============================
// Http2ProtocolHandler tests
// ============================

TEST(Http2ProtocolHandler, Creation) {
  Http2Config config;
  Router router;
  bool handlerCalled = false;

  router.setDefault([&handlerCalled](const HttpRequest& /*req*/) {
    handlerCalled = true;
    return HttpResponse(200);
  });

  auto handler = CreateHttp2ProtocolHandler(config, router);

  ASSERT_NE(handler, nullptr);
  EXPECT_EQ(handler->type(), ProtocolType::Http2);
  EXPECT_FALSE(handlerCalled);  // Handler not called until request is processed
}

TEST(Http2ProtocolHandler, HasNoPendingOutputInitially) {
  Http2Config config;
  Router router;
  auto handler = CreateHttp2ProtocolHandler(config, router);

  EXPECT_FALSE(handler->hasPendingOutput());
}

TEST(Http2ProtocolHandler, ConnectionPreface) {
  Http2Config config;
  Router router;
  auto handler = CreateHttp2ProtocolHandler(config, router);

  // Server waits for client preface before sending its own SETTINGS
  // So initially there should be no pending output
  // (The Http2Connection sends SETTINGS when it receives client preface)
  // This is different from HTTP/2 TLS where server can send preface immediately
  EXPECT_FALSE(handler->hasPendingOutput());
}

TEST(Http2ProtocolHandler, InitiateClose) {
  Http2Config config;
  Router router;
  auto handler = CreateHttp2ProtocolHandler(config, router);

  // Clear any initial output (SETTINGS)
  if (handler->hasPendingOutput()) {
    auto output = handler->getPendingOutput();
    handler->onOutputWritten(output.size());
  }

  handler->initiateClose();

  // After initiateClose(), should have GOAWAY frame queued
  EXPECT_TRUE(handler->hasPendingOutput());

  auto output = handler->getPendingOutput();
  ASSERT_GE(output.size(), 9U);
  EXPECT_EQ(static_cast<uint8_t>(output[3]), 0x07);  // GOAWAY frame type
}

// ============================
// Factory function tests
// ============================

TEST(CreateHttp2ProtocolHandler, ReturnsValidHandler) {
  Http2Config config;
  config.maxConcurrentStreams = 200;
  config.initialWindowSize = 32768;

  Router router;
  router.setDefault(
      [](const HttpRequest& req) { return HttpResponse().status(200).body("Hello from " + std::string(req.path())); });

  auto handler = CreateHttp2ProtocolHandler(config, router);

  ASSERT_NE(handler, nullptr);
  EXPECT_EQ(handler->type(), ProtocolType::Http2);
}

}  // namespace
}  // namespace aeronet::http2
