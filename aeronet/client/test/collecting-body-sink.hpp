#pragma once

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>

#include "aeronet/http-constants.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/response-body-sink.hpp"

namespace aeronet::test {

// ResponseBodySink recording what it receives, optionally stopping the transfer.
class CollectingBodySink final : public ResponseBodySink {
 public:
  bool onHead(const HttpResponse& head, const ResponseBodyInfo& info) override {
    EXPECT_EQ(bodyCalls, 0U) << "onHead() after onBody()";
    ++headCalls;
    status = head.status();
    contentType.assign(info.contentType);
    bodySize = info.size;
    contentEncoding.assign(head.headerValueOrEmpty(http::ContentEncoding));
    headHasBody = !head.bodyInMemory().empty();
    return acceptHead;
  }

  bool onBody(std::string_view chunk) override {
    EXPECT_EQ(headCalls, 1U) << "onBody() without onHead()";
    EXPECT_FALSE(chunk.empty());
    ++bodyCalls;
    largestChunk = std::max(largestChunk, chunk.size());
    body.append(chunk);
    EXPECT_LE(body.size(), bodySize) << "body larger than announced";
    return stopAfterBytes == 0 || body.size() < stopAfterBytes;
  }

  // Behavior
  std::size_t stopAfterBytes{0};  // when non-zero, stop the transfer once the body reaches this size
  bool acceptHead{true};

  // Observations
  std::string body;
  std::string contentType;
  std::string contentEncoding;
  std::size_t headCalls{0};
  std::size_t bodyCalls{0};
  std::size_t largestChunk{0};
  std::size_t bodySize{0};  // ResponseBodyInfo::size given to onHead()
  http::StatusCode status{0};
  bool headHasBody{false};
};

}  // namespace aeronet::test
