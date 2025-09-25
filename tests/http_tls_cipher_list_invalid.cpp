// Test invalid cipher list configuration triggers construction failure.
#include <gtest/gtest.h>

#include <string>

#include "aeronet/http-server-config.hpp"
#include "test_server_tls_fixture.hpp"

TEST(HttpTlsCipherList, InvalidCipherListThrows) {
  EXPECT_THROW(
      { TlsTestServer ts({}, [](aeronet::HttpServerConfig& cfg) { cfg.withTlsCipherList("INVALID-CIPHER-1234"); }); },
      std::runtime_error);
}
