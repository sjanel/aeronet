#include <gtest/gtest.h>

#include <type_traits>

#include "aeronet/features.hpp"
#include "aeronet/multi-http-server.hpp"
#include "aeronet/single-http-server.hpp"
#include "aeronet/version.hpp"

TEST(CompileTimeTraits, StaticChecks) {
  // Compile-time assurances for API ergonomics and client ergonomics.
  static_assert(std::is_nothrow_default_constructible_v<aeronet::SingleHttpServer>);
  static_assert(std::is_move_constructible_v<aeronet::SingleHttpServer>);
  static_assert(std::is_move_assignable_v<aeronet::SingleHttpServer>);

  static_assert(std::is_nothrow_default_constructible_v<aeronet::MultiHttpServer>);
  static_assert(std::is_move_constructible_v<aeronet::MultiHttpServer>);
  static_assert(std::is_move_assignable_v<aeronet::MultiHttpServer>);
}

TEST(CompileTimeTraits, Version) { EXPECT_FALSE(aeronet::version().empty()); }

TEST(CompileTimeTraits, Features) {
#ifdef AERONET_ENABLE_OPENSSL
  EXPECT_TRUE(aeronet::openSslEnabled());
#else
  EXPECT_FALSE(aeronet::openSslEnabled());
#endif

#ifdef AERONET_ENABLE_SPDLOG
  EXPECT_TRUE(aeronet::spdLogEnabled());
#else
  EXPECT_FALSE(aeronet::spdLogEnabled());
#endif

#ifdef AERONET_ENABLE_ZLIB
  EXPECT_TRUE(aeronet::zlibEnabled());
#else
  EXPECT_FALSE(aeronet::zlibEnabled());
#endif

#ifdef AERONET_ENABLE_ZSTD
  EXPECT_TRUE(aeronet::zstdEnabled());
#else
  EXPECT_FALSE(aeronet::zstdEnabled());
#endif

#ifdef AERONET_ENABLE_BROTLI
  EXPECT_TRUE(aeronet::brotliEnabled());
#else
  EXPECT_FALSE(aeronet::brotliEnabled());
#endif

#ifdef AERONET_ENABLE_OPENTELEMETRY
  EXPECT_TRUE(aeronet::openTelemetryEnabled());
#else
  EXPECT_FALSE(aeronet::openTelemetryEnabled());
#endif
}