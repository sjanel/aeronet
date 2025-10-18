#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>

#include "aeronet/http-body.hpp"
#include "aeronet/http-response-data.hpp"
#include "transport.hpp"

using namespace aeronet;

// Fake transport that simulates a partial head write first, then completes remaining head and body.
class PartialWriteTransport : public ITransport {
 public:
  using ITransport::write;  // bring base overloads into scope

  PartialWriteTransport() = default;

  std::size_t read([[maybe_unused]] char* buf, [[maybe_unused]] std::size_t len, TransportHint& want) override {
    want = TransportHint::Error;
    return 0;
  }

  std::size_t write(std::string_view data, TransportHint& want) override {
    // On the very first call we simulate a partial write: write only the first N bytes
    if (!_firstWriteDone) {
      _firstWriteDone = true;
      const std::size_t partial = std::min<std::size_t>(data.size(), 8);
      // pretend we wrote 'partial' bytes
      want = TransportHint::None;  // indicate no special want; still returned >0 bytes means partial progress
      _out.append(data.substr(0, partial));
      return partial;
    }
    // Subsequent calls write everything
    _out.append(data.data(), data.size());
    want = TransportHint::None;
    return data.size();
  }

  [[nodiscard]] std::string_view out() const { return _out; }

 private:
  bool _firstWriteDone{false};
  std::string _out;
};

TEST(PartialHeadWrite, BodyNotSentBeforeHeadPlain) {
  PartialWriteTransport plainWriteTransport;
  HttpResponseData httpResponseData("HTTP/1.1 200 OK\r\nContent-Length: 11\r\n\r\n",
                                    HttpBody(std::string("hello world")));

  TransportHint want;
  // First write will write partial head only
  const auto w1 = plainWriteTransport.write(httpResponseData, want);
  EXPECT_GT(w1, 0U);
  // After first partial write, transport must not have body bytes in output
  std::string_view s1 = plainWriteTransport.out();
  EXPECT_EQ(s1.find("hello world"), std::string_view::npos);

  // Simulate caller retrying: write remaining head then body
  // We expect the remaining head + body to be appended on further writes
  httpResponseData.addOffset(static_cast<std::size_t>(w1));
  const auto w2 = plainWriteTransport.write(httpResponseData, want);
  EXPECT_GT(w2, 0U);
  std::string_view s2 = plainWriteTransport.out();
  // Body must now be present
  EXPECT_NE(s2.find("hello world"), std::string_view::npos);
}

// Reuse the same fake transport semantics to simulate TLS partial write behavior.
TEST(PartialHeadWrite, BodyNotSentBeforeHeadTls) {
  PartialWriteTransport partialWriteTransport;
  HttpResponseData httpResponseData("HTTP/1.1 200 OK\r\nContent-Length: 11\r\n\r\n",
                                    HttpBody(std::string("hello world")));

  TransportHint want;
  const auto w1 = partialWriteTransport.write(httpResponseData, want);
  EXPECT_GT(w1, 0U);
  std::string_view s1 = partialWriteTransport.out();
  EXPECT_EQ(s1.find("hello world"), std::string_view::npos);

  httpResponseData.addOffset(static_cast<std::size_t>(w1));
  const auto w2 = partialWriteTransport.write(httpResponseData, want);
  EXPECT_GT(w2, 0U);
  std::string_view s2 = partialWriteTransport.out();
  EXPECT_NE(s2.find("hello world"), std::string_view::npos);
}
