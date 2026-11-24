// Microbenchmark of WebSocketClient: messages received from, and sent to, a loopback server thread that only streams
// pre-built frames, or discards what it receives, so that the client is the bottleneck.
//
//  - BM_ReceiveTextMessages: one poll() per iteration, the items being the messages delivered to onMessage (read,
//    frame parsing, UTF-8 validation, callback).
//  - BM_ReceiveWithReceive: one WebSocketConnection::receive() per iteration, for a connection without onMessage
//    callback (the messages are copied into its receive queue, the reads pause while they wait).
//  - BM_SendTextMessages: one sendText() out of the callbacks per iteration (frame building, masking, write).
//  - BM_PostTextMessages: batches of postText() (the path of the other threads: copy under a mutex, wakeup), each
//    batch sent by one poll(). The items are the messages.

#include <benchmark/benchmark.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "aeronet/raw-bytes.hpp"
#include "aeronet/websocket-client.hpp"
#include "aeronet/websocket-config.hpp"
#include "aeronet/websocket-constants.hpp"
#include "aeronet/websocket-frame.hpp"
#include "aeronet/websocket-upgrade.hpp"

namespace {

using aeronet::WebSocketClient;
using aeronet::WebSocketConnection;

bool SendAll(int fd, std::string_view data) {
  while (!data.empty()) {
    const auto nbSent = ::send(fd, data.data(), data.size(), MSG_NOSIGNAL);
    if (nbSent <= 0) {
      return false;
    }
    data.remove_prefix(static_cast<std::size_t>(nbSent));
  }
  return true;
}

// Loopback server for one WebSocket connection: it answers the opening handshake, then streams 'frames' in a loop, or
// discards what it receives when there are none, until the client closes the connection.
class FrameServer {
 public:
  explicit FrameServer(std::string frames) : _frames(std::move(frames)) {
    _listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof(addr);
    if (::bind(_listenFd, reinterpret_cast<sockaddr*>(&addr), len) != 0 || ::listen(_listenFd, 1) != 0 ||
        ::getsockname(_listenFd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
      return;
    }
    _port = ntohs(addr.sin_port);
    _thread = std::thread([this] { serve(); });
  }

  FrameServer(const FrameServer&) = delete;
  FrameServer(FrameServer&&) = delete;
  FrameServer& operator=(const FrameServer&) = delete;
  FrameServer& operator=(FrameServer&&) = delete;

  ~FrameServer() {
    if (_thread.joinable()) {
      _thread.join();
    }
    ::close(_listenFd);
  }

  [[nodiscard]] std::string url() const { return "ws://127.0.0.1:" + std::to_string(_port) + "/"; }

 private:
  void serve() {
    const int fd = ::accept(_listenFd, nullptr, nullptr);
    std::string head;
    char buf[1 << 16];
    while (!head.contains("\r\n\r\n")) {
      const auto nbRead = ::recv(fd, buf, sizeof(buf), 0);
      if (nbRead <= 0) {
        ::close(fd);
        return;
      }
      head.append(buf, static_cast<std::size_t>(nbRead));
    }
    // Header names are case-insensitive: the key is looked up in a lowercase copy of the head.
    static constexpr std::string_view kKeyLine = "sec-websocket-key: ";
    std::string lower(head);
    std::ranges::transform(lower, lower.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    const auto accept =
        aeronet::ComputeWebSocketAccept(std::string_view(head).substr(lower.find(kKeyLine) + kKeyLine.size(), 24));
    std::string response = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n";
    response.append("Sec-WebSocket-Accept: ").append(accept.data(), accept.size()).append("\r\n\r\n");
    if (SendAll(fd, response)) {
      if (_frames.empty()) {
        while (::recv(fd, buf, sizeof(buf), 0) > 0) {
        }
      } else {
        while (SendAll(fd, _frames)) {
        }
      }
    }
    ::close(fd);
  }

  std::string _frames;
  std::thread _thread;
  int _listenFd{-1};
  uint16_t _port{0};
};

aeronet::websocket::WebSocketConfig BenchConfig() {
  aeronet::websocket::WebSocketConfig config;
#ifdef AERONET_ENABLE_ZLIB
  config.deflateConfig.enabled = false;
#endif
  return config;
}

// About 1 MiB of text frames of 'payloadSize' bytes, streamed by a FrameServer.
std::string StreamedFrames(std::size_t payloadSize) {
  aeronet::RawBytes frame;
  const std::string payload(payloadSize, 'k');
  aeronet::websocket::BuildFrame(frame, aeronet::websocket::Opcode::Text, std::as_bytes(std::span(payload)));
  std::string frames;
  while (frames.size() < (1UL << 20U)) {
    frames.append(reinterpret_cast<const char*>(frame.data()), frame.size());
  }
  return frames;
}

void BM_ReceiveTextMessages(benchmark::State& state) {
  const auto payloadSize = static_cast<std::size_t>(state.range(0));
  const FrameServer server(StreamedFrames(payloadSize));
  WebSocketClient client(BenchConfig());
  int64_t nbMessages = 0;
  const auto result =
      client.connect(server.url(),
                     {.onOpen = {},
                      .onMessage =
                          [](WebSocketConnection& cnx, std::span<const std::byte> message, bool isBinary) {
                            benchmark::DoNotOptimize(message.data());
                            benchmark::DoNotOptimize(isBinary);
                            ++*static_cast<int64_t*>(cnx.userData());
                          },
                      .onPong = {},
                      .onClose = {}},
                     &nbMessages);
  if (!result) {
    state.SkipWithError(result.pErrorMessage);
    return;
  }
  for ([[maybe_unused]] auto iter : state) {
    client.poll(std::chrono::milliseconds{100});
  }
  state.SetItemsProcessed(nbMessages);
  state.SetBytesProcessed(nbMessages * static_cast<int64_t>(payloadSize));
}

BENCHMARK(BM_ReceiveTextMessages)->Arg(16)->Arg(128)->Arg(1024);

void BM_ReceiveWithReceive(benchmark::State& state) {
  const auto payloadSize = static_cast<std::size_t>(state.range(0));
  const FrameServer server(StreamedFrames(payloadSize));
  WebSocketClient client(BenchConfig());
  // Without onMessage: the messages are kept for receive().
  const auto result = client.connect(server.url(), {});
  if (!result) {
    state.SkipWithError(result.pErrorMessage);
    return;
  }
  WebSocketConnection& cnx = *result.pConnection;
  for ([[maybe_unused]] auto iter : state) {
    const aeronet::WebSocketReceiveResult received = cnx.receive(std::chrono::milliseconds{100});
    if (!received) {
      state.SkipWithError("no message");
      break;
    }
    benchmark::DoNotOptimize(received.payload.data());
  }
  state.SetItemsProcessed(state.iterations());
  state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(payloadSize));
}

BENCHMARK(BM_ReceiveWithReceive)->Arg(16)->Arg(128)->Arg(1024);

void BM_SendTextMessages(benchmark::State& state) {
  const FrameServer server({});
  WebSocketClient client(BenchConfig());
  const auto result = client.connect(server.url(), {});
  if (!result) {
    state.SkipWithError(result.pErrorMessage);
    return;
  }
  WebSocketConnection& cnx = *result.pConnection;
  const std::string payload(static_cast<std::size_t>(state.range(0)), 's');
  for ([[maybe_unused]] auto iter : state) {
    if (!cnx.sendText(payload)) {
      state.SkipWithError("connection closed");
      break;
    }
    if (cnx.bufferedAmount() != 0) {
      client.poll(std::chrono::milliseconds{100});  // the server reads slower: wait for the socket
    }
  }
  state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_SendTextMessages)->Arg(16)->Arg(128)->Arg(1024);

void BM_PostTextMessages(benchmark::State& state) {
  static constexpr int kBatchSize = 64;
  const FrameServer server({});
  WebSocketClient client(BenchConfig());
  const auto result = client.connect(server.url(), {});
  if (!result) {
    state.SkipWithError(result.pErrorMessage);
    return;
  }
  WebSocketConnection& cnx = *result.pConnection;
  const uint64_t id = cnx.id();
  const std::string payload(static_cast<std::size_t>(state.range(0)), 'p');
  for ([[maybe_unused]] auto iter : state) {
    for (int idx = 0; idx < kBatchSize; ++idx) {
      client.postText(id, payload);
    }
    // One wakeup for the batch: poll() takes the queue at once, and writes the messages together.
    client.poll(std::chrono::milliseconds{100});
    while (cnx.bufferedAmount() != 0) {
      client.poll(std::chrono::milliseconds{100});  // the server reads slower: wait for the socket
    }
  }
  state.SetItemsProcessed(state.iterations() * kBatchSize);
}

BENCHMARK(BM_PostTextMessages)->Arg(16)->Arg(128)->Arg(1024);

}  // namespace

BENCHMARK_MAIN();
