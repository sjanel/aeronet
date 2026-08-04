#pragma once

#include <string_view>

#include "aeronet/http2-process-result-error-msg.hpp"

namespace aeronet::http2 {

constexpr std::string_view ConvertProcessResultErrorMsgToSv(ErrorMsg msg) {
  switch (msg) {
    case ErrorMsg::NoError:
      return "No Error";
    case ErrorMsg::InvalidConnectionPreface:
      return "Invalid Connection Preface";
    case ErrorMsg::FrameExceedsMaximumSize:
      return "Frame Exceeds Maximum Size";
    case ErrorMsg::ExpectedCONTINUATIONFrame:
      return "Expected CONTINUATION Frame";
    case ErrorMsg::UnexpectedPUSH_PROMISE:
      return "Unexpected PUSH_PROMISE";
    case ErrorMsg::DATAFrameOnStreamZero:
      return "DATA Frame On Stream Zero";
    case ErrorMsg::InvalidPaddingInDATAFrame:
      return "Invalid Padding In DATA Frame";
    case ErrorMsg::InvalidDATAFrame:
      return "Invalid DATA Frame";
    case ErrorMsg::DATAOnClosedStream:
      return "DATA On Closed Stream";
    case ErrorMsg::ConnectionFlowControlExceeded:
      return "Connection Flow Control Exceeded";
    case ErrorMsg::HEADERSFrameOnStreamZero:
      return "HEADERS Frame On Stream Zero";
    case ErrorMsg::InvalidPaddingInHEADERSFrame:
      return "Invalid Padding In HEADERS Frame";
    case ErrorMsg::InvalidHEADERSFrame:
      return "Invalid HEADERS Frame";
    case ErrorMsg::ServerInitiatedStreamIdFromClient:
      return "Server Initiated Stream Id From Client";
    case ErrorMsg::StreamIdNotIncreasing:
      return "Stream Id Not Increasing";
    case ErrorMsg::StreamFlowControlExceeded:
      return "Stream Flow Control Exceeded";
    case ErrorMsg::StreamDependsOnItself:
      return "Stream Depends On Itself";
    case ErrorMsg::MaxConcurrentStreamsExceeded:
      return "Max Concurrent Streams Exceeded";
    case ErrorMsg::HeaderBlockTooLarge:
      return "Header Block Too Large";
    case ErrorMsg::HeaderListTooLarge:
      return "Header List Too Large";
    case ErrorMsg::InvalidStreamStateForHEADERS:
      return "Invalid Stream State For HEADERS";
    case ErrorMsg::HPACKDecodingFailed:
      return "HPACK Decoding Failed";
    case ErrorMsg::PRIORITYFrameOnStreamZero:
      return "PRIORITY Frame On Stream Zero";
    case ErrorMsg::InvalidPRIORITYFrame:
      return "Invalid PRIORITY Frame";
    case ErrorMsg::TooManyPRIORITYFramesOnIdleStreams:
      return "Too Many PRIORITY Frames On Idle Streams";
    case ErrorMsg::RST_STREAMFrameOnStreamZero:
      return "RST_STREAM Frame On Stream Zero";
    case ErrorMsg::InvalidRST_STREAMFrame:
      return "Invalid RST_STREAM Frame";
    case ErrorMsg::SETTINGSFrameOnNonZeroStream:
      return "SETTINGS Frame On Non Zero Stream";
    case ErrorMsg::InvalidSETTINGSFrame:
      return "Invalid SETTINGS Frame";
    case ErrorMsg::InvalidENABLE_PUSHValue:
      return "Invalid ENABLE_PUSH Value";
    case ErrorMsg::InitialWindowSizeTooLarge:
      return "Initial Window Size Too Large";
    case ErrorMsg::WindowSizeUpdateOverflow:
      return "Window Size Update Overflow";
    case ErrorMsg::InvalidMAX_FRAMESize:
      return "Invalid MAX_FRAME Size";
    case ErrorMsg::PINGFrameOnNonZeroStream:
      return "PING Frame On Non Zero Stream";
    case ErrorMsg::InvalidPINGFrame:
      return "Invalid PING Frame";
    case ErrorMsg::GOAWAYFrameOnNonZeroStream:
      return "GOAWAY Frame On Non Zero Stream";
    case ErrorMsg::InvalidGOAWAYFrame:
      return "Invalid GOAWAY Frame";
    case ErrorMsg::InvalidWINDOW_UPDATEFrame:
      return "Invalid WINDOW_UPDATE Frame";
    case ErrorMsg::ZeroWINDOW_UPDATEIncrement:
      return "Zero WINDOW_UPDATE Increment";
    case ErrorMsg::ZeroWINDOW_UPDATEIncrementOnConnection:
      return "Zero WINDOW_UPDATE Increment On Connection";
    case ErrorMsg::ConnectionWindowOverflow:
      return "Connection Window Overflow";
    case ErrorMsg::StreamWindowOverflow:
      return "Stream Window Overflow";
    case ErrorMsg::CONTINUATIONOnWrongStream:
      return "CONTINUATION On Wrong Stream";
    case ErrorMsg::StreamNotFoundForCONTINUATION:
      return "Stream Not Found For CONTINUATION";
    case ErrorMsg::UnexpectedCONTINUATIONFrame:
      return "Unexpected CONTINUATION Frame";
    default:
      return "Unknown Error";
  }
}

}  // namespace aeronet::http2