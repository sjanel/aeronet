#pragma once

#include <string_view>

#include "aeronet/http2-frame-types.hpp"

namespace aeronet::http2 {

constexpr std::string_view FrameTypeName(FrameType type) noexcept {
  switch (type) {
    case FrameType::Data:
      return "data";
    case FrameType::Headers:
      return "headers";
    case FrameType::Priority:
      return "priority";
    case FrameType::RstStream:
      return "rst_stream";
    case FrameType::Settings:
      return "settings";
    case FrameType::PushPromise:
      return "push_promise";
    case FrameType::Ping:
      return "ping";
    case FrameType::GoAway:
      return "goaway";
    case FrameType::WindowUpdate:
      return "window_update";
    case FrameType::Continuation:
      return "continuation";
    default:
      return "unknown";
  }
}

}  // namespace aeronet::http2