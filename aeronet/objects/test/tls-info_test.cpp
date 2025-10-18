#include "tls-info.hpp"

#include <gtest/gtest.h>

#include <string>

using namespace aeronet;

TEST(TLSInfo, DefaultConstructedEmpty) {
  TLSInfo info;
  EXPECT_EQ(info.selectedAlpn(), std::string_view());
  EXPECT_EQ(info.negotiatedCipher(), std::string_view());
  EXPECT_EQ(info.negotiatedVersion(), std::string_view());
}

TEST(TLSInfo, ParameterizedStoresAndReturns) {
  TLSInfo info("h2", "TLS_AES_128_GCM_SHA256", "TLSv1.3");
  EXPECT_EQ(info.selectedAlpn(), "h2");
  EXPECT_EQ(info.negotiatedCipher(), "TLS_AES_128_GCM_SHA256");
  EXPECT_EQ(info.negotiatedVersion(), "TLSv1.3");
}

TEST(TLSInfo, LongStringsAreHandled) {
  std::string alpn(1000, 'A');
  std::string cipher(500, 'B');
  std::string version(200, 'C');
  TLSInfo info(alpn, cipher, version);
  EXPECT_EQ(info.selectedAlpn(), std::string_view(alpn));
  EXPECT_EQ(info.negotiatedCipher(), std::string_view(cipher));
  EXPECT_EQ(info.negotiatedVersion(), std::string_view(version));
}

TEST(TLSInfo, CopyAndAssign) {
  TLSInfo src("proto", "cipher", "v1");
  TLSInfo copyInfo = src;  // NOLINT(performance-unnecessary-copy-initialization)
  EXPECT_EQ(copyInfo.selectedAlpn(), "proto");
  EXPECT_EQ(copyInfo.negotiatedCipher(), "cipher");
  EXPECT_EQ(copyInfo.negotiatedVersion(), "v1");

  TLSInfo dst;
  dst = src;  // copy assign
  EXPECT_EQ(dst.selectedAlpn(), "proto");
  EXPECT_EQ(dst.negotiatedCipher(), "cipher");
  EXPECT_EQ(dst.negotiatedVersion(), "v1");
}
