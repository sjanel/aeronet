#pragma once

#include <string_view>

#include "aeronet/http2-frame-types.hpp"

namespace aeronet::http2 {

/// Convert ErrorCode to human-readable string for logging/debugging.
constexpr std::string_view ErrorCodeName(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::NoError:
      return "NO_ERROR";
    case ErrorCode::ProtocolError:
      return "PROTOCOL_ERROR";
    case ErrorCode::InternalError:
      return "INTERNAL_ERROR";
    case ErrorCode::FlowControlError:
      return "FLOW_CONTROL_ERROR";
    case ErrorCode::SettingsTimeout:
      return "SETTINGS_TIMEOUT";
    case ErrorCode::StreamClosed:
      return "STREAM_CLOSED";
    case ErrorCode::FrameSizeError:
      return "FRAME_SIZE_ERROR";
    case ErrorCode::RefusedStream:
      return "REFUSED_STREAM";
    case ErrorCode::Cancel:
      return "CANCEL";
    case ErrorCode::CompressionError:
      return "COMPRESSION_ERROR";
    case ErrorCode::ConnectError:
      return "CONNECT_ERROR";
    case ErrorCode::EnhanceYourCalm:
      return "ENHANCE_YOUR_CALM";
    case ErrorCode::InadequateSecurity:
      return "INADEQUATE_SECURITY";
    case ErrorCode::Http11Required:
      return "HTTP_1_1_REQUIRED";
    default:
      return "UNKNOWN_ERROR";
  }
}

}  // namespace aeronet::http2