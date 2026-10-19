// Portable (Linux, macOS, Windows) checks of the interest set semantics of EventLoop over a real TCP connection:
// mod() replaces the previous interest set, as epoll_ctl(EPOLL_CTL_MOD) does, on every backend.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <span>

#include "aeronet/adaptive-poll-timeout.hpp"
#include "aeronet/connection.hpp"
#include "aeronet/event-loop.hpp"
#include "aeronet/event.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/socket-ops.hpp"
#include "aeronet/socket.hpp"
#include "aeronet/tcp-connector.hpp"

#ifdef AERONET_POSIX
#include <sys/socket.h>
#elifdef AERONET_WINDOWS
#include <winsock2.h>
#endif

namespace aeronet {

namespace {

// Polls once (20 ms timeout) and returns the union of the events reported for 'fd', 0 when none.
EventBmp PollOnce(EventLoop& loop, NativeHandle fd) {
  EventBmp bmp = 0;
  const auto events = loop.poll();
  EXPECT_NE(events.data(), nullptr);
  for (const auto& event : events) {
    if (event.fd == fd) {
      bmp |= event.eventBmp;
    }
  }
  return bmp;
}

// Polls until an event is reported for 'fd' (bytes sent over loopback may not be readable immediately), up to 5 s.
EventBmp PollUntilEvent(EventLoop& loop, NativeHandle fd) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
  EventBmp bmp = 0;
  while (bmp == 0 && std::chrono::steady_clock::now() < deadline) {
    bmp = PollOnce(loop, fd);
  }
  return bmp;
}

// A connected TCP loopback pair: the client side is watched by the event loop, the server side feeds it.
class EventLoopInterestTest : public ::testing::Test {
 protected:
  void SetUp() override {
    uint16_t port = 0;
    _listener.bindAndListen(false, port);
    char host[] = "127.0.0.1";
    _client = ConnectTCP(std::span<char>(host, sizeof(host) - 1U), port, 0, 5000);
    ASSERT_FALSE(_client.failure);
    ASSERT_FALSE(_client.connectPending);
    sockaddr_storage peerAddress{};
    _server = Connection(_listener, peerAddress);
    ASSERT_TRUE(_client.cnx && _server);
  }

  [[nodiscard]] NativeHandle clientFd() const noexcept { return _client.cnx.fd(); }

  void sendByteToClient() {
    const char byte = 'x';
    ASSERT_EQ(SafeSend(_server.fd(), &byte, 1), 1);
  }

  EventLoop _loop{
      PollTimeoutPolicy{.baseTimeout = std::chrono::milliseconds{20}, .minFactor = 1.0F, .maxFactor = 1.0F}};
  Socket _listener{Socket::Type::Stream};
  ConnectResult _client;
  Connection _server;
};

}  // namespace

// A write interest absent from the new mask stops being reported, although the socket stays writable.
TEST_F(EventLoopInterestTest, ModDropsWriteInterest) {
  ASSERT_TRUE(_loop.add(EventLoop::EventFd{clientFd(), EventIn | EventOut}));
  EXPECT_EQ(PollUntilEvent(_loop, clientFd()), EventOut);  // writable, nothing to read

  ASSERT_TRUE(_loop.mod(EventLoop::EventFd{clientFd(), EventIn}));
  EXPECT_EQ(PollOnce(_loop, clientFd()), 0U);

  sendByteToClient();
  EXPECT_EQ(PollUntilEvent(_loop, clientFd()), EventIn);
}

// A read interest absent from the new mask stops being reported, although unread input is pending.
TEST_F(EventLoopInterestTest, ModDropsReadInterest) {
  ASSERT_TRUE(_loop.add(EventLoop::EventFd{clientFd(), EventIn}));
  sendByteToClient();
  EXPECT_EQ(PollUntilEvent(_loop, clientFd()), EventIn);

  ASSERT_TRUE(_loop.mod(EventLoop::EventFd{clientFd(), EventOut}));
  EXPECT_EQ(PollUntilEvent(_loop, clientFd()), EventOut);

  ASSERT_TRUE(_loop.mod(EventLoop::EventFd{clientFd(), EventIn}));
  EXPECT_EQ(PollUntilEvent(_loop, clientFd()), EventIn);  // the byte is still unread
}

#ifndef AERONET_WINDOWS  // WSAPoll has no edge-triggered mode
// The server waits for a full socket send buffer to drain by adding an edge-triggered write interest, and drops it
// once flushed: each new write interest must report a writable socket once, even after it was dropped.
TEST_F(EventLoopInterestTest, EdgeTriggeredWriteInterestReportsWritableSocketAtEachMod) {
  ASSERT_TRUE(_loop.add(EventLoop::EventFd{clientFd(), EventIn | EventEt}));
  EXPECT_EQ(PollOnce(_loop, clientFd()), 0U);
  for (int round = 0; round < 2; ++round) {
    ASSERT_TRUE(_loop.mod(EventLoop::EventFd{clientFd(), EventIn | EventOut | EventEt}));
    EXPECT_EQ(PollUntilEvent(_loop, clientFd()), EventOut);
    EXPECT_EQ(PollOnce(_loop, clientFd()), 0U);  // reported once only

    ASSERT_TRUE(_loop.mod(EventLoop::EventFd{clientFd(), EventIn | EventEt}));
    EXPECT_EQ(PollOnce(_loop, clientFd()), 0U);
  }
}
#endif

}  // namespace aeronet
