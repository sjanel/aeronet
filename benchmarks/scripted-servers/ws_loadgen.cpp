// ws_loadgen.cpp - Native WebSocket load generator for the scripted server benchmarks.
//
// k6 spends tens of microseconds of CPU per WebSocket message (JavaScript per message), several times more than an
// efficient server: on a small machine it cannot saturate even a single server thread, so it measures itself rather
// than the server. This generator does the minimum work per message (one masked frame write, one frame parse) with
// one epoll loop per thread, so that a few threads can keep a server thread busy.
//
// Each connection performs the WebSocket handshake, then keeps --pipeline messages in flight: every reply (echo or
// pong) is answered by the next message. In churn mode, each session sends one message, waits for its echo, closes
// the connection and reconnects. Results are printed as a single JSON line on stdout: latencies are the message round
// trips (echo / pong), or the session durations from connect to close in churn mode, and cpu_s is the CPU time used by
// the generator during the measurement window (to check that it was not the bottleneck).

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifdef WS_LOADGEN_HAVE_ZLIB
#include <zlib.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;

enum class Mode : std::uint8_t { Echo, Ping, Mix, Churn };

enum class Phase : std::uint8_t { Warmup, Measure, Stop };

struct Options {
  std::string host{"127.0.0.1"};
  std::string path{"/ws-uncompressed"};
  uint16_t port{8080};
  uint32_t connections{50};
  uint32_t threads{1};
  uint32_t pipeline{1};
  uint32_t payloadSize{128};
  double durationS{10.0};
  double warmupS{2.0};
  Mode mode{Mode::Echo};
  bool binary{false};
  bool compress{false};
  bool jsonPayload{false};
};

constexpr uint8_t kOpContinuation = 0x0;
constexpr uint8_t kOpText = 0x1;
constexpr uint8_t kOpBinary = 0x2;
constexpr uint8_t kOpClose = 0x8;
constexpr uint8_t kOpPing = 0x9;
constexpr uint8_t kOpPong = 0xA;

std::atomic<Phase> gPhase{Phase::Warmup};

[[noreturn]] void Die(const char* msg) {
  std::fprintf(stderr, "ws_loadgen: %s\n", msg);
  std::exit(2);
}

// Log-linear latency histogram (16 sub-buckets per power of two of microseconds): ~6% precision, constant memory.
class Histogram {
 public:
  void record(uint64_t micros) {
    ++_buckets[Index(micros)];
    ++_count;
    _sum += micros;
    _max = std::max(_max, micros);
  }

  void merge(const Histogram& other) {
    for (std::size_t idx = 0; idx < kNbBuckets; ++idx) {
      _buckets[idx] += other._buckets[idx];
    }
    _count += other._count;
    _sum += other._sum;
    _max = std::max(_max, other._max);
  }

  [[nodiscard]] double percentile(double pct) const {
    if (_count == 0) {
      return 0.0;
    }
    const auto target = static_cast<uint64_t>(pct / 100.0 * static_cast<double>(_count - 1)) + 1;
    uint64_t seen = 0;
    for (std::size_t idx = 0; idx < kNbBuckets; ++idx) {
      seen += _buckets[idx];
      if (seen >= target) {
        return static_cast<double>(std::min(UpperBound(idx), _max));
      }
    }
    return static_cast<double>(_max);
  }

  [[nodiscard]] double avg() const {
    return _count == 0 ? 0.0 : static_cast<double>(_sum) / static_cast<double>(_count);
  }
  [[nodiscard]] uint64_t max() const { return _max; }

 private:
  static constexpr std::size_t kSubBits = 4;
  static constexpr std::size_t kNbBuckets = 64 << kSubBits;

  static std::size_t Index(uint64_t value) {
    if (value < (1U << kSubBits)) {
      return static_cast<std::size_t>(value);
    }
    const auto msb = static_cast<std::size_t>(std::bit_width(value) - 1);
    const auto sub = static_cast<std::size_t>((value >> (msb - kSubBits)) & ((1U << kSubBits) - 1));
    return ((msb - kSubBits + 1) << kSubBits) + sub;
  }

  static uint64_t UpperBound(std::size_t idx) {
    if (idx < (1U << kSubBits)) {
      return idx;
    }
    const std::size_t msb = (idx >> kSubBits) + kSubBits - 1;
    const uint64_t sub = idx & ((1U << kSubBits) - 1);
    return ((uint64_t{1} << kSubBits) + sub + 1) << (msb - kSubBits);
  }

  std::array<uint64_t, kNbBuckets> _buckets{};
  uint64_t _count{0};
  uint64_t _sum{0};
  uint64_t _max{0};
};

struct Payload {
  std::string bytes;  // message payload as sent on the wire (compressed when permessage-deflate is in use)
  uint8_t opcode{kOpText};
  bool compressed{false};
};

std::string MakePayloadBytes(const Options& opts, bool binary, uint32_t size) {
  if (opts.jsonPayload) {
    // Same document as k6/ws_compression.js: highly compressible JSON (~1.5 KB).
    std::string json = R"({"type":"benchmark","data":[)";
    for (int idx = 0; idx < 20; ++idx) {
      if (idx != 0) {
        json += ',';
      }
      json += R"({"id":)" + std::to_string(idx) + R"(,"name":"item-)" + std::to_string(idx) + R"(","value":)" +
              std::to_string(idx * 100) + R"(,"tags":["benchmark","test","compression"]})";
    }
    json += "]}";
    return json;
  }
  std::string bytes(size, 'A');
  if (binary) {
    for (uint32_t idx = 0; idx < size; ++idx) {
      bytes[idx] = static_cast<char>(idx & 0xFFU);
    }
  }
  return bytes;
}

#ifdef WS_LOADGEN_HAVE_ZLIB
// permessage-deflate (RFC 7692) with no context takeover: every message compresses to the same bytes.
std::string Deflate(std::string_view input) {
  z_stream stream{};
  if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
    Die("deflateInit2 failed");
  }
  std::string out(deflateBound(&stream, static_cast<uLong>(input.size())) + 16, '\0');
  stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
  stream.avail_in = static_cast<uInt>(input.size());
  stream.next_out = reinterpret_cast<Bytef*>(out.data());
  stream.avail_out = static_cast<uInt>(out.size());
  if (deflate(&stream, Z_SYNC_FLUSH) != Z_OK) {
    Die("deflate failed");
  }
  out.resize(out.size() - stream.avail_out);
  deflateEnd(&stream);
  // Strip the 0x00 0x00 0xFF 0xFF tail of the sync flush (RFC 7692 section 7.2.1).
  if (out.size() >= 4 && out.ends_with(std::string_view("\x00\x00\xff\xff", 4))) {
    out.resize(out.size() - 4);
  }
  return out;
}
#endif

std::vector<Payload> MakePayloads(const Options& opts, bool compressionNegotiated) {
  std::vector<Payload> payloads;
  const auto add = [&](bool binary, uint32_t size) {
    Payload payload{MakePayloadBytes(opts, binary, size), binary ? kOpBinary : kOpText, false};
#ifdef WS_LOADGEN_HAVE_ZLIB
    if (compressionNegotiated) {
      payload.bytes = Deflate(payload.bytes);
      payload.compressed = true;
    }
#else
    (void)compressionNegotiated;
#endif
    payloads.push_back(std::move(payload));
  };
  switch (opts.mode) {
    case Mode::Ping:
      payloads.push_back(Payload{std::string{}, kOpPing, false});
      break;
    case Mode::Mix:
      add(false, 256);
      add(true, 512);
      break;
    case Mode::Churn:
      payloads.push_back(Payload{std::string("churn-test"), kOpText, false});
      break;
    case Mode::Echo:
      add(opts.binary, opts.payloadSize);
      break;
  }
  return payloads;
}

struct ThreadStats {
  Histogram latency;
  uint64_t messages{0};
  uint64_t sessions{0};
  uint64_t errors{0};
  uint64_t compressionRefused{0};
};

enum class ConnState : std::uint8_t { Connecting, Handshaking, Open, Closing };

struct Connection {
  int fd{-1};
  ConnState state{ConnState::Connecting};
  bool waitingWritable{false};
  bool compression{false};
  uint32_t nextPayload{0};
  Clock::time_point sessionStart;
  std::string in;
  std::size_t inOffset{0};
  std::string out;
  std::size_t outOffset{0};
  // Send timestamps of the in-flight messages (replies come back in order).
  std::vector<Clock::time_point> inflight;
  std::size_t inflightHead{0};
  std::size_t inflightCount{0};
};

class Worker {
 public:
  Worker(const Options& opts, const sockaddr_in& addr, uint32_t nbConnections, uint64_t seed)
      : _opts(opts), _addr(addr), _rng(seed | 1U), _conns(nbConnections) {
    _epollFd = ::epoll_create1(EPOLL_CLOEXEC);
    if (_epollFd < 0) {
      Die("epoll_create1 failed");
    }
    _payloads[0] = MakePayloads(opts, false);
    _payloads[1] = MakePayloads(opts, true);
    // Fixed key: the generator does not validate Sec-WebSocket-Accept, only the 101 status.
    _handshake = "GET " + opts.path + " HTTP/1.1\r\nHost: " + opts.host + ":" + std::to_string(opts.port) +
                 "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==" +
                 "\r\nSec-WebSocket-Version: 13\r\n";
    if (opts.compress) {
      _handshake +=
          "Sec-WebSocket-Extensions: permessage-deflate; client_no_context_takeover; "
          "server_no_context_takeover\r\n";
    }
    _handshake += "\r\n";
  }

  Worker(const Worker&) = delete;
  Worker& operator=(const Worker&) = delete;

  ~Worker() {
    for (auto& conn : _conns) {
      if (conn.fd >= 0) {
        ::close(conn.fd);
      }
    }
    if (_epollFd >= 0) {
      ::close(_epollFd);
    }
  }

  void run() {
    for (std::size_t idx = 0; idx < _conns.size(); ++idx) {
      connect(idx);
    }
    std::array<epoll_event, 256> events{};
    while (gPhase.load(std::memory_order_relaxed) != Phase::Stop) {
      const int nb = ::epoll_wait(_epollFd, events.data(), static_cast<int>(events.size()), 10);
      if (nb < 0) {
        if (errno == EINTR) {
          continue;
        }
        Die("epoll_wait failed");
      }
      _measuring = gPhase.load(std::memory_order_relaxed) == Phase::Measure;
      for (int evIdx = 0; evIdx < nb; ++evIdx) {
        const auto idx = static_cast<std::size_t>(events[static_cast<std::size_t>(evIdx)].data.u64);
        const uint32_t mask = events[static_cast<std::size_t>(evIdx)].events;
        handleEvent(idx, mask);
      }
    }
  }

  [[nodiscard]] const ThreadStats& stats() const { return _stats; }

 private:
  void connect(std::size_t idx) {
    Connection& conn = _conns[idx];
    conn = Connection{};
    conn.inflight.resize(std::max<uint32_t>(1, _opts.pipeline));
    conn.sessionStart = Clock::now();
    conn.fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (conn.fd < 0) {
      Die("socket failed");
    }
    const int one = 1;
    ::setsockopt(conn.fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    const int rc = ::connect(conn.fd, reinterpret_cast<const sockaddr*>(&_addr), sizeof(_addr));
    if (rc != 0 && errno != EINPROGRESS) {
      fail(idx);
      return;
    }
    epoll_event ev{};
    ev.events = EPOLLOUT;
    ev.data.u64 = idx;
    if (::epoll_ctl(_epollFd, EPOLL_CTL_ADD, conn.fd, &ev) != 0) {
      Die("epoll_ctl ADD failed");
    }
    conn.waitingWritable = true;
  }

  void fail(std::size_t idx) {
    if (_measuring) {
      ++_stats.errors;
    }
    reconnect(idx);
  }

  void reconnect(std::size_t idx) {
    Connection& conn = _conns[idx];
    if (conn.fd >= 0) {
      ::close(conn.fd);  // also removes it from the epoll set
      conn.fd = -1;
    }
    if (gPhase.load(std::memory_order_relaxed) != Phase::Stop) {
      connect(idx);
    }
  }

  void setWritableInterest(std::size_t idx, bool enable) {
    Connection& conn = _conns[idx];
    if (conn.waitingWritable == enable) {
      return;
    }
    epoll_event ev{};
    ev.events = enable ? (EPOLLIN | EPOLLOUT) : EPOLLIN;
    ev.data.u64 = idx;
    ::epoll_ctl(_epollFd, EPOLL_CTL_MOD, conn.fd, &ev);
    conn.waitingWritable = enable;
  }

  void handleEvent(std::size_t idx, uint32_t mask) {
    Connection& conn = _conns[idx];
    if (conn.fd < 0) {
      return;
    }
    if (conn.state == ConnState::Connecting) {
      int err = 0;
      socklen_t len = sizeof(err);
      if ((mask & (EPOLLERR | EPOLLHUP)) != 0 || ::getsockopt(conn.fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 ||
          err != 0) {
        fail(idx);
        return;
      }
      conn.state = ConnState::Handshaking;
      setWritableInterest(idx, false);
      queue(idx, _handshake);
      return;
    }
    if ((mask & EPOLLOUT) != 0 && !flush(idx)) {
      return;
    }
    if ((mask & (EPOLLIN | EPOLLERR | EPOLLHUP)) != 0) {
      readAndProcess(idx);
    }
  }

  // Appends bytes to the connection output and writes as much as possible. Returns false if the connection failed.
  bool queue(std::size_t idx, std::string_view bytes) {
    Connection& conn = _conns[idx];
    conn.out.append(bytes);
    return flush(idx);
  }

  bool flush(std::size_t idx) {
    Connection& conn = _conns[idx];
    while (conn.outOffset < conn.out.size()) {
      const auto nb = ::send(conn.fd, conn.out.data() + conn.outOffset, conn.out.size() - conn.outOffset, MSG_NOSIGNAL);
      if (nb > 0) {
        conn.outOffset += static_cast<std::size_t>(nb);
        continue;
      }
      if (nb < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        setWritableInterest(idx, true);
        return true;
      }
      if (nb < 0 && errno == EINTR) {
        continue;
      }
      fail(idx);
      return false;
    }
    conn.out.clear();
    conn.outOffset = 0;
    setWritableInterest(idx, false);
    return true;
  }

  uint32_t nextMask() {
    // xorshift32: masking keys only need to vary (RFC 6455 section 5.3), not to be cryptographically strong here.
    _rng ^= _rng << 13U;
    _rng ^= _rng >> 17U;
    _rng ^= _rng << 5U;
    return _rng;
  }

  void appendFrame(std::string& out, uint8_t opcode, bool rsv1, std::string_view payload) {
    std::array<uint8_t, 14> header{};
    std::size_t headerSize = 2;
    header[0] = static_cast<uint8_t>(0x80U | (rsv1 ? 0x40U : 0U) | opcode);
    const std::size_t len = payload.size();
    if (len < 126) {
      header[1] = static_cast<uint8_t>(0x80U | len);
    } else if (len <= 0xFFFF) {
      header[1] = 0x80U | 126U;
      header[2] = static_cast<uint8_t>(len >> 8U);
      header[3] = static_cast<uint8_t>(len);
      headerSize = 4;
    } else {
      header[1] = 0x80U | 127U;
      for (std::size_t byte = 0; byte < 8; ++byte) {
        header[2 + byte] = static_cast<uint8_t>(static_cast<uint64_t>(len) >> (8U * (7U - byte)));
      }
      headerSize = 10;
    }
    const uint32_t maskKey = nextMask();
    std::memcpy(header.data() + headerSize, &maskKey, 4);
    headerSize += 4;
    const std::size_t start = out.size();
    out.resize(start + headerSize + len);
    char* dst = out.data() + start;
    std::memcpy(dst, header.data(), headerSize);
    dst += headerSize;
    // Mask 8 bytes at a time, then the tail.
    const uint64_t mask64 = (static_cast<uint64_t>(maskKey) << 32U) | maskKey;
    std::size_t pos = 0;
    for (; pos + 8 <= len; pos += 8) {
      uint64_t word;
      std::memcpy(&word, payload.data() + pos, 8);
      word ^= mask64;
      std::memcpy(dst + pos, &word, 8);
    }
    const auto* maskBytes = reinterpret_cast<const uint8_t*>(&maskKey);
    for (; pos < len; ++pos) {
      dst[pos] = static_cast<char>(static_cast<uint8_t>(payload[pos]) ^ maskBytes[pos & 3U]);
    }
  }

  bool sendMessage(std::size_t idx) {
    Connection& conn = _conns[idx];
    const auto& payloads = _payloads[conn.compression ? 1 : 0];
    const Payload& payload = payloads[conn.nextPayload % payloads.size()];
    ++conn.nextPayload;
    if (conn.inflightCount < conn.inflight.size()) {
      conn.inflight[(conn.inflightHead + conn.inflightCount) % conn.inflight.size()] = Clock::now();
      ++conn.inflightCount;
    }
    appendFrame(conn.out, payload.opcode, payload.compressed, payload.bytes);
    return flush(idx);
  }

  void onOpen(std::size_t idx) {
    Connection& conn = _conns[idx];
    conn.state = ConnState::Open;
    const uint32_t initial = _opts.mode == Mode::Churn ? 1U : std::max<uint32_t>(1, _opts.pipeline);
    for (uint32_t msg = 0; msg < initial; ++msg) {
      if (!sendMessage(idx)) {
        return;
      }
    }
  }

  // Returns false if the connection was recycled.
  bool onReply(std::size_t idx) {
    Connection& conn = _conns[idx];
    if (conn.inflightCount != 0) {
      if (_measuring && _opts.mode != Mode::Churn) {
        recordLatency(conn.inflight[conn.inflightHead]);
      }
      conn.inflightHead = (conn.inflightHead + 1) % conn.inflight.size();
      --conn.inflightCount;
    }
    if (_measuring) {
      ++_stats.messages;
    }
    if (_opts.mode == Mode::Churn) {
      // Close the session (status 1000) and wait for the server close frame before reconnecting.
      conn.state = ConnState::Closing;
      static constexpr std::array<char, 2> kNormalClosure{static_cast<char>(0x03), static_cast<char>(0xE8)};
      appendFrame(conn.out, kOpClose, false, std::string_view(kNormalClosure.data(), kNormalClosure.size()));
      return flush(idx);
    }
    return sendMessage(idx);
  }

  void recordLatency(Clock::time_point since) {
    _stats.latency.record(
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - since).count()));
  }

  void endSession(std::size_t idx) {
    if (_measuring) {
      ++_stats.sessions;
      recordLatency(_conns[idx].sessionStart);
    }
    reconnect(idx);
  }

  bool processHandshake(std::size_t idx) {
    Connection& conn = _conns[idx];
    const auto headerEnd = conn.in.find("\r\n\r\n");
    if (headerEnd == std::string::npos) {
      if (conn.in.size() < 16384) {
        return true;  // wait for the rest of the response head
      }
      fail(idx);
      return false;
    }
    const std::string_view head(conn.in.data(), headerEnd);
    if (!head.starts_with("HTTP/1.1 101")) {
      fail(idx);
      return false;
    }
    std::string lower(head);
    std::ranges::transform(lower, lower.begin(), [](char ch) { return static_cast<char>(std::tolower(ch)); });
    // Compression is in use only if the server accepted it in its Sec-WebSocket-Extensions response header.
    const auto extPos = lower.find("\r\nsec-websocket-extensions:");
    const auto extEnd = extPos == std::string::npos ? std::string::npos : lower.find("\r\n", extPos + 2);
    conn.compression = _opts.compress && extPos != std::string::npos &&
                       std::string_view(lower).substr(extPos, extEnd - extPos).contains("permessage-deflate");
    if (_opts.compress && !conn.compression) {
      ++_stats.compressionRefused;
    }
    conn.inOffset = headerEnd + 4;
    onOpen(idx);
    return conn.fd >= 0;
  }

  void readAndProcess(std::size_t idx) {
    Connection& conn = _conns[idx];
    std::array<char, 64 * 1024> buf;
    for (;;) {
      const auto nb = ::recv(conn.fd, buf.data(), buf.size(), 0);
      if (nb > 0) {
        conn.in.append(buf.data(), static_cast<std::size_t>(nb));
        if (static_cast<std::size_t>(nb) < buf.size()) {
          break;
        }
        continue;
      }
      if (nb == 0) {
        // Peer closed: the end of a churn session, an error otherwise.
        if (conn.state == ConnState::Closing) {
          endSession(idx);
        } else {
          fail(idx);
        }
        return;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        break;
      }
      if (errno == EINTR) {
        continue;
      }
      fail(idx);
      return;
    }
    if (conn.state == ConnState::Handshaking && !processHandshake(idx)) {
      return;
    }
    processFrames(idx);
  }

  void processFrames(std::size_t idx) {
    Connection& conn = _conns[idx];
    while (conn.fd >= 0 && conn.state != ConnState::Handshaking) {
      const std::size_t avail = conn.in.size() - conn.inOffset;
      if (avail < 2) {
        break;
      }
      const auto* data = reinterpret_cast<const uint8_t*>(conn.in.data() + conn.inOffset);
      const bool fin = (data[0] & 0x80U) != 0;
      const uint8_t opcode = data[0] & 0x0FU;
      const bool masked = (data[1] & 0x80U) != 0;
      uint64_t len = data[1] & 0x7FU;
      std::size_t headerSize = 2;
      if (len == 126) {
        if (avail < 4) {
          break;
        }
        len = (static_cast<uint64_t>(data[2]) << 8U) | data[3];
        headerSize = 4;
      } else if (len == 127) {
        if (avail < 10) {
          break;
        }
        len = 0;
        for (std::size_t byte = 0; byte < 8; ++byte) {
          len = (len << 8U) | data[2 + byte];
        }
        headerSize = 10;
      }
      if (masked) {
        headerSize += 4;
      }
      if (avail < headerSize + len) {
        break;
      }
      conn.inOffset += headerSize + static_cast<std::size_t>(len);
      bool alive = true;
      switch (opcode) {
        case kOpText:
        case kOpBinary:
        case kOpContinuation:
          if (fin && _opts.mode != Mode::Ping) {
            alive = onReply(idx);
          }
          break;
        case kOpPong:
          if (_opts.mode == Mode::Ping) {
            alive = onReply(idx);
          }
          break;
        case kOpPing:
          appendFrame(conn.out, kOpPong, false, {});
          alive = flush(idx);
          break;
        case kOpClose:
          if (conn.state == ConnState::Closing) {
            endSession(idx);
          } else {
            fail(idx);
          }
          return;
        default:
          fail(idx);
          return;
      }
      if (!alive) {
        return;
      }
    }
    if (conn.fd >= 0 && conn.inOffset != 0) {
      conn.in.erase(0, conn.inOffset);
      conn.inOffset = 0;
    }
  }

  const Options& _opts;
  sockaddr_in _addr;
  uint32_t _rng;
  int _epollFd{-1};
  bool _measuring{false};
  std::string _handshake;
  std::array<std::vector<Payload>, 2> _payloads;  // [0] uncompressed, [1] permessage-deflate
  std::vector<Connection> _conns;
  ThreadStats _stats;
};

double ParseDuration(std::string_view text) {
  double factor = 1.0;
  if (text.ends_with("ms")) {
    factor = 0.001;
    text.remove_suffix(2);
  } else if (text.ends_with('s')) {
    text.remove_suffix(1);
  } else if (text.ends_with('m')) {
    factor = 60.0;
    text.remove_suffix(1);
  }
  return std::strtod(std::string(text).c_str(), nullptr) * factor;
}

// User + system CPU time consumed so far by all the threads of this process.
double ProcessCpuSeconds() {
  rusage usage{};
  ::getrusage(RUSAGE_SELF, &usage);
  const auto seconds = [](const timeval& tv) { return static_cast<double>(tv.tv_sec) + 1e-6 * tv.tv_usec; };
  return seconds(usage.ru_utime) + seconds(usage.ru_stime);
}

uint32_t ParseUint(std::string_view text) {
  uint32_t value = 0;
  const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (ec != std::errc{} || ptr != text.data() + text.size()) {
    Die("invalid integer argument");
  }
  return value;
}

Options ParseOptions(int argc, char** argv) {
  Options opts;
  for (int argPos = 1; argPos < argc; ++argPos) {
    const std::string_view arg(argv[argPos]);
    const auto next = [&]() -> std::string_view {
      if (argPos + 1 >= argc) {
        Die("missing option value");
      }
      return argv[++argPos];
    };
    if (arg == "--host") {
      opts.host = next();
    } else if (arg == "--port") {
      opts.port = static_cast<uint16_t>(ParseUint(next()));
    } else if (arg == "--path") {
      opts.path = next();
    } else if (arg == "--connections" || arg == "-c") {
      opts.connections = ParseUint(next());
    } else if (arg == "--threads" || arg == "-t") {
      opts.threads = ParseUint(next());
    } else if (arg == "--pipeline") {
      opts.pipeline = ParseUint(next());
    } else if (arg == "--payload-size") {
      opts.payloadSize = ParseUint(next());
    } else if (arg == "--duration" || arg == "-d") {
      opts.durationS = ParseDuration(next());
    } else if (arg == "--warmup") {
      opts.warmupS = ParseDuration(next());
    } else if (arg == "--binary") {
      opts.binary = true;
    } else if (arg == "--json-payload") {
      opts.jsonPayload = true;
    } else if (arg == "--compress") {
      opts.compress = true;
    } else if (arg == "--mode") {
      const std::string_view mode = next();
      if (mode == "echo") {
        opts.mode = Mode::Echo;
      } else if (mode == "ping") {
        opts.mode = Mode::Ping;
      } else if (mode == "mix") {
        opts.mode = Mode::Mix;
      } else if (mode == "churn") {
        opts.mode = Mode::Churn;
      } else {
        Die("unknown --mode (echo, ping, mix, churn)");
      }
    } else if (arg == "--help" || arg == "-h") {
      std::printf(
          "Usage: ws_loadgen [--host H] [--port P] [--path /ws] [-c connections] [-t threads] [-d duration]\n"
          "                  [--warmup duration] [--mode echo|ping|mix|churn] [--payload-size N] [--binary]\n"
          "                  [--json-payload] [--compress] [--pipeline N]\n");
      std::exit(0);
    } else {
      Die("unknown option (see --help)");
    }
  }
  opts.threads = std::max<uint32_t>(1, std::min(opts.threads, std::max<uint32_t>(1, opts.connections)));
#ifndef WS_LOADGEN_HAVE_ZLIB
  if (opts.compress) {
    Die("--compress requires zlib (not available in this build)");
  }
#endif
  return opts;
}

}  // namespace

int main(int argc, char** argv) {
  const Options opts = ParseOptions(argc, argv);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(opts.port);
  if (::inet_pton(AF_INET, opts.host.c_str(), &addr.sin_addr) != 1) {
    Die("--host must be an IPv4 address");
  }

  std::vector<std::unique_ptr<Worker>> workers;
  for (uint32_t threadPos = 0; threadPos < opts.threads; ++threadPos) {
    const uint32_t nbConnections = opts.connections / opts.threads + (threadPos < opts.connections % opts.threads);
    workers.push_back(std::make_unique<Worker>(opts, addr, nbConnections, 0x9E3779B9U * (threadPos + 1)));
  }
  std::vector<std::thread> threads;
  threads.reserve(workers.size());
  for (auto& worker : workers) {
    threads.emplace_back([&worker] { worker->run(); });
  }

  std::this_thread::sleep_for(std::chrono::duration<double>(opts.warmupS));
  gPhase.store(Phase::Measure, std::memory_order_relaxed);
  const auto start = Clock::now();
  const double startCpuS = ProcessCpuSeconds();
  std::this_thread::sleep_for(std::chrono::duration<double>(opts.durationS));
  gPhase.store(Phase::Stop, std::memory_order_relaxed);
  const double elapsed = std::chrono::duration<double>(Clock::now() - start).count();
  const double cpuS = ProcessCpuSeconds() - startCpuS;
  for (auto& thread : threads) {
    thread.join();
  }

  ThreadStats total;
  for (const auto& worker : workers) {
    const ThreadStats& stats = worker->stats();
    total.latency.merge(stats.latency);
    total.messages += stats.messages;
    total.sessions += stats.sessions;
    total.errors += stats.errors;
    total.compressionRefused += stats.compressionRefused;
  }
  std::printf(
      "{\"messages\":%llu,\"sessions\":%llu,\"errors\":%llu,\"duration_s\":%.3f,\"cpu_s\":%.3f,\"rate\":%.1f,"
      "\"sessions_rate\":%.1f,\"compression_refused\":%llu,\"latency_us\":{\"avg\":%.1f,\"p50\":%.0f,\"p90\":%.0f,"
      "\"p95\":%.0f,\"p99\":%.0f,\"max\":%llu}}\n",
      static_cast<unsigned long long>(total.messages), static_cast<unsigned long long>(total.sessions),
      static_cast<unsigned long long>(total.errors), elapsed, cpuS, static_cast<double>(total.messages) / elapsed,
      static_cast<double>(total.sessions) / elapsed, static_cast<unsigned long long>(total.compressionRefused),
      total.latency.avg(), total.latency.percentile(50), total.latency.percentile(90), total.latency.percentile(95),
      total.latency.percentile(99), static_cast<unsigned long long>(total.latency.max()));
  return 0;
}
