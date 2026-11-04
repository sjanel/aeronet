#pragma once

#include <cstdint>
#include <string_view>

#include "aeronet/http-request-view.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/raw-chars.hpp"

namespace aeronet::test {

// Creates requests for the tests of the code taking an HttpRequestView, outside of a server (its constructor is
// private).
struct HttpRequestViewTestAccess {
  // The status of the parsing of a head not complete yet (see HttpRequestView::initTrySetHead()).
  static constexpr http::StatusCode kStatusNeedMoreData = HttpRequestView::kStatusNeedMoreData;

  [[nodiscard]] static HttpRequestView Make() { return {}; }

  // Parses a copy of 'input', received at once, as the head of the first request of a connection. Returns the status of
  // HttpRequestView::initTrySetHead(): kStatusNeedMoreData if the head is not complete yet.
  [[nodiscard]] static http::StatusCode ParseHead(std::string_view input, uint32_t maxHeaderBytes) {
    RawChars inBuffer(input);
    RawChars tmpBuffer;
    HttpRequestView request;
    HttpRequestView::HeadParseState headParseState = HttpRequestView::HeadParseState::None;
    return request.initTrySetHead(inBuffer, headParseState, tmpBuffer, maxHeaderBytes, true, nullptr);
  }
};

}  // namespace aeronet::test
