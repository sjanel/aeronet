#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <string>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response-writer.hpp"
#include "aeronet/test_server_tls_fixture.hpp"
#include "aeronet/test_tls_client.hpp"

TEST(HttpTlsStreamingBackpressure, LargeChunksTls) {
  aeronet::test::TlsTestServer ts({"http/1.1"}, nullptr, std::chrono::milliseconds{20});
  // Create large chunks to exercise TLS partial writes
  static constexpr std::size_t kChunkSize = 65536;
  static constexpr int kNbChunks = 32;

  std::string chunk(kChunkSize, 'X');
  ts.setDefault([&](const aeronet::HttpRequest&, aeronet::HttpResponseWriter& writer) {
    writer.statusCode(200);
    writer.contentType("text/plain");
    // write several large chunks
    for (int chunkPos = 0; chunkPos < kNbChunks; ++chunkPos) {
      writer.writeBody(chunk);
    }
    writer.end();
  });

  aeronet::test::TlsClient client(ts.port());
  auto raw = client.get("/large", {});
  ASSERT_FALSE(raw.empty());
  // Response should contain a sizable body; simple sanity: expect more than one chunk size marker or body length
  EXPECT_GT(raw.size(), kChunkSize * static_cast<std::size_t>(kNbChunks));
}
