#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>

#include "aeronet/http-payload.hpp"
#include "aeronet/http-response-data.hpp"
#include "aeronet/transport.hpp"

using namespace aeronet;

// Fake transport that simulates a partial head write first, then completes remaining head and body.
class PartialWriteTransport : public ITransport {
 public:
  using ITransport::write;  // bring base overloads into scope

  PartialWriteTransport() = default;

  TransportResult read([[maybe_unused]] char* buf, [[maybe_unused]] std::size_t len) override {
    return {0, TransportHint::Error};
  }

  TransportResult write(std::string_view data) override {
    // On the very first call we simulate a partial write: write only the first N bytes
    TransportResult ret{data.size(), TransportHint::None};
    if (_firstWriteDone) {  // Subsequent calls write everything
      _out.append(data);
    } else {
      _firstWriteDone = true;
      ret.bytesProcessed = std::min<std::size_t>(data.size(), 8);
      // pretend we wrote 'partial' bytes
      _out.append(data.substr(0, ret.bytesProcessed));
    }

    return ret;
  }

  [[nodiscard]] std::string_view out() const { return _out; }

 private:
  bool _firstWriteDone{false};
  std::string _out;
};

TEST(PartialHeadWrite, BodyNotSentBeforeHeadPlain) {
  PartialWriteTransport plainWriteTransport;
  HttpResponseData httpResponseData("HTTP/1.1 200 OK\r\nContent-Length: 11\r\n\r\n",
                                    HttpPayload(std::string("hello world")));

  // First write will write partial head only
  const auto [w1, want1] = plainWriteTransport.write(httpResponseData.firstBuffer(), httpResponseData.secondBuffer());
  EXPECT_GT(w1, 0U);
  // After first partial write, transport must not have body bytes in output
  std::string_view s1 = plainWriteTransport.out();
  EXPECT_FALSE(s1.contains("hello world"));

  // Simulate caller retrying: write remaining head then body
  // We expect the remaining head + body to be appended on further writes
  httpResponseData.addOffset(static_cast<std::size_t>(w1));
  const auto [w2, want2] = plainWriteTransport.write(httpResponseData.firstBuffer(), httpResponseData.secondBuffer());
  EXPECT_GT(w2, 0U);
  std::string_view s2 = plainWriteTransport.out();
  // Body must now be present
  EXPECT_TRUE(s2.contains("hello world"));
}

// Reuse the same fake transport semantics to simulate TLS partial write behavior.
TEST(PartialHeadWrite, BodyNotSentBeforeHeadTls) {
  PartialWriteTransport partialWriteTransport;
  HttpResponseData httpResponseData("HTTP/1.1 200 OK\r\nContent-Length: 11\r\n\r\n",
                                    HttpPayload(std::string("hello world")));

  const auto [w1, want1] = partialWriteTransport.write(httpResponseData.firstBuffer(), httpResponseData.secondBuffer());
  EXPECT_GT(w1, 0U);
  std::string_view s1 = partialWriteTransport.out();
  EXPECT_FALSE(s1.contains("hello world"));

  httpResponseData.addOffset(static_cast<std::size_t>(w1));
  const auto [w2, want2] = partialWriteTransport.write(httpResponseData.firstBuffer(), httpResponseData.secondBuffer());
  EXPECT_GT(w2, 0U);
  std::string_view s2 = partialWriteTransport.out();
  EXPECT_TRUE(s2.contains("hello world"));
}
