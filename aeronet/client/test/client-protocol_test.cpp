// Unit coverage for the ClientProtocol enum + ALPN identifier mapping helpers.
#include "aeronet/client-protocol.hpp"

#include <gtest/gtest.h>

namespace aeronet {

TEST(ClientProtocolTest, ToAlpnId) {
  EXPECT_EQ(ToAlpnId(ClientProtocol::Http1_1), "http/1.1");
  EXPECT_EQ(ToAlpnId(ClientProtocol::Http2), "h2");
}

TEST(ClientProtocolTest, FromAlpnIdKnown) {
  EXPECT_EQ(ClientProtocolFromAlpnId("http/1.1"), ClientProtocol::Http1_1);
  EXPECT_EQ(ClientProtocolFromAlpnId("h2"), ClientProtocol::Http2);
}

TEST(ClientProtocolTest, FromAlpnIdUnknownOrEmptyFallsBackToHttp11) {
  EXPECT_EQ(ClientProtocolFromAlpnId(""), ClientProtocol::Http1_1);
  EXPECT_EQ(ClientProtocolFromAlpnId("spdy/3.1"), ClientProtocol::Http1_1);
  EXPECT_EQ(ClientProtocolFromAlpnId("h3"), ClientProtocol::Http1_1);
  // Round-trips for the protocols we actually name.
  EXPECT_EQ(ClientProtocolFromAlpnId(ToAlpnId(ClientProtocol::Http1_1)), ClientProtocol::Http1_1);
  EXPECT_EQ(ClientProtocolFromAlpnId(ToAlpnId(ClientProtocol::Http2)), ClientProtocol::Http2);
}

}  // namespace aeronet
