#include <gtest/gtest.h>

#include <type_traits>

#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "aeronet/multi-http-server.hpp"

TEST(CompileTimeTraits, StaticChecks) {
  // Compile-time assurances for API ergonomics and client ergonomics.
  static_assert(std::is_nothrow_default_constructible_v<aeronet::HttpServer>);
  static_assert(std::is_move_constructible_v<aeronet::HttpServer>);
  static_assert(std::is_move_assignable_v<aeronet::HttpServer>);

  static_assert(std::is_nothrow_default_constructible_v<aeronet::MultiHttpServer>);
  static_assert(std::is_move_constructible_v<aeronet::MultiHttpServer>);
  static_assert(std::is_move_assignable_v<aeronet::MultiHttpServer>);

  // HttpServerConfig should be an aggregate for convenient initialization by callers.
  static_assert(std::is_aggregate_v<aeronet::HttpServerConfig>);
}
