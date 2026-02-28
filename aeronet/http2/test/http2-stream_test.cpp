#include "aeronet/http2-stream.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <type_traits>

#include "aeronet/http2-frame-types.hpp"

namespace aeronet::http2 {

namespace {

// ============================
// Initial State Tests
// ============================

TEST(Http2Stream, InitialState) {
  Http2Stream stream(1, kDefaultInitialWindowSize);

  EXPECT_EQ(stream.id(), 1U);
  EXPECT_EQ(stream.state(), StreamState::Idle);
  EXPECT_FALSE(stream.isClosed());
  EXPECT_FALSE(stream.canSend());
  EXPECT_FALSE(stream.canReceive());
  EXPECT_EQ(stream.sendWindow(), static_cast<int32_t>(kDefaultInitialWindowSize));
  EXPECT_EQ(stream.recvWindow(), static_cast<int32_t>(kDefaultInitialWindowSize));
  EXPECT_EQ(stream.weight(), 16U);
  EXPECT_EQ(stream.streamDependency(), 0);
  EXPECT_FALSE(stream.isExclusive());
}

// ============================
// State Transition: Idle -> Open
// ============================

TEST(Http2Stream, TransitionIdleToOpen_RecvHeaders) {
  Http2Stream stream(1, kDefaultInitialWindowSize);

  ErrorCode err = stream.onRecvHeaders(false);

  EXPECT_EQ(err, ErrorCode::NoError);
  EXPECT_EQ(stream.state(), StreamState::Open);
  EXPECT_TRUE(stream.canSend());
  EXPECT_TRUE(stream.canReceive());
}

TEST(Http2Stream, TransitionIdleToHalfClosedRemote_RecvHeadersEndStream) {
  Http2Stream stream(1, kDefaultInitialWindowSize);

  ErrorCode err = stream.onRecvHeaders(true);  // END_STREAM set

  EXPECT_EQ(err, ErrorCode::NoError);
  EXPECT_EQ(stream.state(), StreamState::HalfClosedRemote);
  EXPECT_TRUE(stream.canSend());
  EXPECT_FALSE(stream.canReceive());
}

TEST(Http2Stream, TransitionOpenToHalfClosedRemote_RecvHeadersEndStream) {
  Http2Stream stream(1, kDefaultInitialWindowSize);
  ASSERT_EQ(stream.onRecvHeaders(false), ErrorCode::NoError);
  ASSERT_EQ(stream.state(), StreamState::Open);

  const ErrorCode err = stream.onRecvHeaders(true);

  EXPECT_EQ(err, ErrorCode::NoError);
  EXPECT_EQ(stream.state(), StreamState::HalfClosedRemote);
}

TEST(Http2Stream, RecvHeadersInOpenStateWithoutEndStreamDoesNotChangeState) {
  Http2Stream stream(1, kDefaultInitialWindowSize);
  ASSERT_EQ(stream.onRecvHeaders(false), ErrorCode::NoError);
  ASSERT_EQ(stream.state(), StreamState::Open);

  const ErrorCode err = stream.onRecvHeaders(false);

  EXPECT_EQ(err, ErrorCode::NoError);
  EXPECT_EQ(stream.state(), StreamState::Open);
}

TEST(Http2Stream, TransitionIdleToOpen_SendHeaders) {
  Http2Stream stream(2, kDefaultInitialWindowSize);  // Even ID = server-initiated

  ErrorCode err = stream.onSendHeaders(false);

  EXPECT_EQ(err, ErrorCode::NoError);
  EXPECT_EQ(stream.state(), StreamState::Open);
}

TEST(Http2Stream, TransitionIdleToHalfClosedLocal_SendHeadersEndStream) {
  Http2Stream stream(2, kDefaultInitialWindowSize);

  ErrorCode err = stream.onSendHeaders(true);  // END_STREAM set

  EXPECT_EQ(err, ErrorCode::NoError);
  EXPECT_EQ(stream.state(), StreamState::HalfClosedLocal);
  EXPECT_FALSE(stream.canSend());
  EXPECT_TRUE(stream.canReceive());
}

TEST(Http2Stream, TransitionOpenToHalfClosedLocal_SendHeadersEndStream) {
  Http2Stream stream(2, kDefaultInitialWindowSize);
  ASSERT_EQ(stream.onSendHeaders(false), ErrorCode::NoError);
  ASSERT_EQ(stream.state(), StreamState::Open);

  const ErrorCode err = stream.onSendHeaders(true);

  EXPECT_EQ(err, ErrorCode::NoError);
  EXPECT_EQ(stream.state(), StreamState::HalfClosedLocal);
}

TEST(Http2Stream, SendHeadersInOpenStateWithoutEndStreamDoesNotChangeState) {
  Http2Stream stream(2, kDefaultInitialWindowSize);
  ASSERT_EQ(stream.onSendHeaders(false), ErrorCode::NoError);
  ASSERT_EQ(stream.state(), StreamState::Open);

  const ErrorCode err = stream.onSendHeaders(false);

  EXPECT_EQ(err, ErrorCode::NoError);
  EXPECT_EQ(stream.state(), StreamState::Open);
}

// ============================
// State Transition: Open -> Half-Closed
// ============================

TEST(Http2Stream, TransitionOpenToHalfClosedRemote_RecvEndStream) {
  Http2Stream stream(1, kDefaultInitialWindowSize);
  EXPECT_EQ(stream.onRecvHeaders(false), ErrorCode::NoError);  // Idle -> Open
  ASSERT_EQ(stream.state(), StreamState::Open);

  ErrorCode err = stream.onRecvData(true);  // END_STREAM

  EXPECT_EQ(err, ErrorCode::NoError);
  EXPECT_EQ(stream.state(), StreamState::HalfClosedRemote);
}

TEST(Http2Stream, TransitionOpenToHalfClosedLocal_SendEndStream) {
  Http2Stream stream(1, kDefaultInitialWindowSize);
  EXPECT_EQ(stream.onRecvHeaders(false), ErrorCode::NoError);  // Idle -> Open
  ASSERT_EQ(stream.state(), StreamState::Open);

  ErrorCode err = stream.onSendData(true);  // END_STREAM

  EXPECT_EQ(err, ErrorCode::NoError);
  EXPECT_EQ(stream.state(), StreamState::HalfClosedLocal);
}

// ============================
// State Transition: Half-Closed -> Closed
// ============================

TEST(Http2Stream, TransitionHalfClosedRemoteToClosed) {
  Http2Stream stream(1, kDefaultInitialWindowSize);
  EXPECT_EQ(stream.onRecvHeaders(true), ErrorCode::NoError);  // Idle -> HalfClosedRemote
  ASSERT_EQ(stream.state(), StreamState::HalfClosedRemote);

  ErrorCode err = stream.onSendData(true);  // END_STREAM

  EXPECT_EQ(err, ErrorCode::NoError);
  EXPECT_EQ(stream.state(), StreamState::Closed);
  EXPECT_TRUE(stream.isClosed());
}

TEST(Http2Stream, TransitionHalfClosedLocalToClosed) {
  Http2Stream stream(1, kDefaultInitialWindowSize);
  EXPECT_EQ(stream.onRecvHeaders(false), ErrorCode::NoError);  // Idle -> Open
  EXPECT_EQ(stream.onSendData(true), ErrorCode::NoError);      // Open -> HalfClosedLocal
  ASSERT_EQ(stream.state(), StreamState::HalfClosedLocal);

  ErrorCode err = stream.onRecvData(true);  // END_STREAM

  EXPECT_EQ(err, ErrorCode::NoError);
  EXPECT_EQ(stream.state(), StreamState::Closed);
}

// ============================
// RST_STREAM Transitions
// ============================

TEST(Http2Stream, TransitionToClosedOnSendRstStream) {
  Http2Stream stream(1, kDefaultInitialWindowSize);
  EXPECT_EQ(stream.onRecvHeaders(false), ErrorCode::NoError);  // Idle -> Open
  ASSERT_EQ(stream.state(), StreamState::Open);

  stream.onSendRstStream();

  EXPECT_EQ(stream.state(), StreamState::Closed);
}

TEST(Http2Stream, TransitionToClosedOnRecvRstStream) {
  Http2Stream stream(1, kDefaultInitialWindowSize);
  EXPECT_EQ(stream.onRecvHeaders(false), ErrorCode::NoError);  // Idle -> Open
  ASSERT_EQ(stream.state(), StreamState::Open);

  stream.onRecvRstStream();

  EXPECT_EQ(stream.state(), StreamState::Closed);
}

// ============================
// Invalid Transitions
// ============================

TEST(Http2Stream, InvalidTransition_DataOnIdle) {
  Http2Stream stream(1, kDefaultInitialWindowSize);

  ErrorCode err = stream.onRecvData(false);

  EXPECT_EQ(err, ErrorCode::StreamClosed);
  EXPECT_EQ(stream.state(), StreamState::Idle);  // State unchanged
}

TEST(Http2Stream, InvalidTransition_DataOnClosed) {
  Http2Stream stream(1, kDefaultInitialWindowSize);
  EXPECT_EQ(stream.onRecvHeaders(true), ErrorCode::NoError);  // Idle -> HalfClosedRemote
  EXPECT_EQ(stream.onSendData(true), ErrorCode::NoError);     // HalfClosedRemote -> Closed
  ASSERT_EQ(stream.state(), StreamState::Closed);

  ErrorCode err = stream.onRecvData(false);

  EXPECT_EQ(err, ErrorCode::StreamClosed);
}

TEST(Http2Stream, InvalidTransition_SendOnHalfClosedLocal) {
  Http2Stream stream(1, kDefaultInitialWindowSize);
  EXPECT_EQ(stream.onRecvHeaders(false), ErrorCode::NoError);  // Idle -> Open
  EXPECT_EQ(stream.onSendData(true), ErrorCode::NoError);      // Open -> HalfClosedLocal
  ASSERT_EQ(stream.state(), StreamState::HalfClosedLocal);

  ErrorCode err = stream.onSendData(false);

  EXPECT_EQ(err, ErrorCode::StreamClosed);
}

TEST(Http2Stream, InvalidTransition_RecvOnHalfClosedRemote) {
  Http2Stream stream(1, kDefaultInitialWindowSize);
  EXPECT_EQ(stream.onRecvHeaders(true), ErrorCode::NoError);  // Idle -> HalfClosedRemote
  ASSERT_EQ(stream.state(), StreamState::HalfClosedRemote);

  ErrorCode err = stream.onRecvData(false);

  EXPECT_EQ(err, ErrorCode::StreamClosed);
}

TEST(Http2Stream, InvalidTransition_SendHeadersOnClosed) {
  Http2Stream stream(1, kDefaultInitialWindowSize);
  stream.onSendRstStream();
  ASSERT_EQ(stream.state(), StreamState::Closed);

  const ErrorCode err = stream.onSendHeaders(false);

  EXPECT_EQ(err, ErrorCode::StreamClosed);
}

TEST(Http2Stream, InvalidTransition_RecvHeadersOnClosed) {
  Http2Stream stream(1, kDefaultInitialWindowSize);
  stream.onRecvRstStream();
  ASSERT_EQ(stream.state(), StreamState::Closed);

  const ErrorCode err = stream.onRecvHeaders(false);

  EXPECT_EQ(err, ErrorCode::StreamClosed);
}

// ============================
// PUSH_PROMISE Transitions
// ============================

TEST(Http2Stream, SendPushPromiseAllowedOnOpenAndHalfClosedRemote) {
  Http2Stream openStream(1, kDefaultInitialWindowSize);
  ASSERT_EQ(openStream.onRecvHeaders(false), ErrorCode::NoError);
  ASSERT_EQ(openStream.state(), StreamState::Open);
  EXPECT_EQ(openStream.onSendPushPromise(), ErrorCode::NoError);

  Http2Stream halfClosedRemoteStream(1, kDefaultInitialWindowSize);
  ASSERT_EQ(halfClosedRemoteStream.onRecvHeaders(true), ErrorCode::NoError);
  ASSERT_EQ(halfClosedRemoteStream.state(), StreamState::HalfClosedRemote);
  EXPECT_EQ(halfClosedRemoteStream.onSendPushPromise(), ErrorCode::NoError);
}

TEST(Http2Stream, SendPushPromiseInvalidStateReturnsProtocolError) {
  Http2Stream stream(1, kDefaultInitialWindowSize);
  ASSERT_EQ(stream.state(), StreamState::Idle);
  EXPECT_EQ(stream.onSendPushPromise(), ErrorCode::ProtocolError);
}

TEST(Http2Stream, RecvPushPromiseAllowedOnOpenAndHalfClosedLocal) {
  Http2Stream openStream(1, kDefaultInitialWindowSize);
  ASSERT_EQ(openStream.onRecvHeaders(false), ErrorCode::NoError);
  ASSERT_EQ(openStream.state(), StreamState::Open);
  EXPECT_EQ(openStream.onRecvPushPromise(), ErrorCode::NoError);

  Http2Stream halfClosedLocalStream(1, kDefaultInitialWindowSize);
  ASSERT_EQ(halfClosedLocalStream.onSendHeaders(true), ErrorCode::NoError);
  ASSERT_EQ(halfClosedLocalStream.state(), StreamState::HalfClosedLocal);
  EXPECT_EQ(halfClosedLocalStream.onRecvPushPromise(), ErrorCode::NoError);
}

TEST(Http2Stream, RecvPushPromiseInvalidStateReturnsProtocolError) {
  Http2Stream stream(1, kDefaultInitialWindowSize);
  stream.onRecvRstStream();
  ASSERT_EQ(stream.state(), StreamState::Closed);
  EXPECT_EQ(stream.onRecvPushPromise(), ErrorCode::ProtocolError);
}

// ============================
// Flow Control Tests
// ============================

TEST(Http2Stream, ConsumeSendWindow) {
  Http2Stream stream(1, 1000);

  EXPECT_TRUE(stream.consumeSendWindow(500));
  EXPECT_EQ(stream.sendWindow(), 500);

  EXPECT_TRUE(stream.consumeSendWindow(500));
  EXPECT_EQ(stream.sendWindow(), 0);

  EXPECT_FALSE(stream.consumeSendWindow(1));  // No more window
}

TEST(Http2Stream, ConsumeRecvWindow) {
  Http2Stream stream(1, 1000);

  EXPECT_TRUE(stream.consumeRecvWindow(500));
  EXPECT_EQ(stream.recvWindow(), 500);

  EXPECT_TRUE(stream.consumeRecvWindow(500));
  EXPECT_EQ(stream.recvWindow(), 0);

  EXPECT_FALSE(stream.consumeRecvWindow(1));  // No more window
}

TEST(Http2Stream, IncreaseSendWindow) {
  Http2Stream stream(1, 1000);
  EXPECT_TRUE(stream.consumeSendWindow(500));  // Window = 500
  ASSERT_EQ(stream.sendWindow(), 500);

  ErrorCode err = stream.increaseSendWindow(300);

  EXPECT_EQ(err, ErrorCode::NoError);
  EXPECT_EQ(stream.sendWindow(), 800);
}

TEST(Http2Stream, IncreaseSendWindowOverflow) {
  Http2Stream stream(1, 2147483647);  // Max window size

  ErrorCode err = stream.increaseSendWindow(1);  // Would overflow

  EXPECT_EQ(err, ErrorCode::FlowControlError);
}

TEST(Http2Stream, IncreaseSendWindowZero) {
  Http2Stream stream(1, 1000);

  ErrorCode err = stream.increaseSendWindow(0);

  EXPECT_EQ(err, ErrorCode::ProtocolError);
}

TEST(Http2Stream, IncreaseRecvWindow) {
  Http2Stream stream(1, 1000);
  EXPECT_TRUE(stream.consumeRecvWindow(500));  // Window = 500
  ASSERT_EQ(stream.recvWindow(), 500);

  (void)stream.increaseRecvWindow(300);

  EXPECT_EQ(stream.recvWindow(), 800);
}

TEST(Http2Stream, UpdateInitialWindowSize) {
  Http2Stream stream(1, 1000);
  EXPECT_TRUE(stream.consumeSendWindow(200));  // Window = 800
  ASSERT_EQ(stream.sendWindow(), 800);

  // New initial window is 1500, delta = +500
  ErrorCode err = stream.updateInitialWindowSize(1500);

  EXPECT_EQ(err, ErrorCode::NoError);
  EXPECT_EQ(stream.sendWindow(), 1300);  // 800 + 500
}

TEST(Http2Stream, UpdateInitialWindowSizeDecrease) {
  Http2Stream stream(1, 1000);
  ASSERT_EQ(stream.sendWindow(), 1000);

  // New initial window is 500, delta = -500
  ErrorCode err = stream.updateInitialWindowSize(500);

  EXPECT_EQ(err, ErrorCode::NoError);
  EXPECT_EQ(stream.sendWindow(), 500);  // 1000 - 500
}

TEST(Http2Stream, UpdateInitialWindowSizeOverflow) {
  Http2Stream stream(1, 2147483647);  // Max

  // Try to increase by 1 - would overflow
  ErrorCode err = stream.updateInitialWindowSize(2147483647U + 1);

  EXPECT_EQ(err, ErrorCode::FlowControlError);
}

TEST(Http2Stream, UpdateInitialWindowSizeDecreaseCanUnderflowCurrentSendWindow) {
  Http2Stream stream(1, 1000);
  ASSERT_TRUE(stream.consumeSendWindow(900));
  ASSERT_EQ(stream.sendWindow(), 100);

  // New initial window is 0, delta = -1000 => newWindow would become negative
  const ErrorCode err = stream.updateInitialWindowSize(0);

  EXPECT_EQ(err, ErrorCode::FlowControlError);
  EXPECT_EQ(stream.sendWindow(), 100);
}

// ============================
// Priority Tests
// ============================

TEST(Http2Stream, SetPriority) {
  Http2Stream stream(1, kDefaultInitialWindowSize);

  stream.setPriority(3, 128, true);

  EXPECT_EQ(stream.streamDependency(), 3U);
  EXPECT_EQ(stream.weight(), 128U);
  EXPECT_TRUE(stream.isExclusive());
}

TEST(Http2Stream, DefaultPriority) {
  Http2Stream stream(1, kDefaultInitialWindowSize);

  EXPECT_EQ(stream.streamDependency(), 0U);
  EXPECT_EQ(stream.weight(), 16U);  // Default weight
  EXPECT_FALSE(stream.isExclusive());
}

// ============================
// Error Code Tests
// ============================

TEST(Http2Stream, SetErrorCode) {
  Http2Stream stream(1, kDefaultInitialWindowSize);

  EXPECT_EQ(stream.errorCode(), ErrorCode::NoError);

  stream.setErrorCode(ErrorCode::Cancel);
  EXPECT_EQ(stream.errorCode(), ErrorCode::Cancel);
}

// ============================
// Stream State Name Tests
// ============================

TEST(Http2Stream, StreamStateName) {
  EXPECT_EQ(StreamStateName(StreamState::Idle), "idle");
  EXPECT_EQ(StreamStateName(StreamState::Open), "open");
  EXPECT_EQ(StreamStateName(StreamState::HalfClosedLocal), "half-closed (local)");
  EXPECT_EQ(StreamStateName(StreamState::HalfClosedRemote), "half-closed (remote)");
  EXPECT_EQ(StreamStateName(StreamState::Closed), "closed");
  EXPECT_EQ(StreamStateName(StreamState::ReservedLocal), "reserved (local)");
  EXPECT_EQ(StreamStateName(StreamState::ReservedRemote), "reserved (remote)");

  EXPECT_EQ(StreamStateName(static_cast<StreamState>(static_cast<std::underlying_type_t<StreamState>>(-1))), "unknown");
}

}  // namespace
}  // namespace aeronet::http2