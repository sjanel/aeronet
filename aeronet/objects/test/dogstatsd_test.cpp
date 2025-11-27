#include "aeronet/dogstatsd.hpp"

#include <gtest/gtest.h>
#include <poll.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>

#include "aeronet/base-fd.hpp"

namespace aeronet {
namespace {

class UnixDogstatsdSink {
 public:
  UnixDogstatsdSink() : _fd(::socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK, 0)) {
    if (!_fd) {
      throw std::runtime_error("Failed to create unix datagram socket");
    }
    _path = makeUniquePath();
    ::unlink(_path.c_str());

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, _path.c_str(), sizeof(addr.sun_path) - 1);
    const socklen_t addrlen = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + _path.size() + 1);
    if (::bind(_fd.fd(), reinterpret_cast<sockaddr*>(&addr), addrlen) != 0) {
      auto err = errno;
      throw std::runtime_error(std::string("bind failed: ") + std::strerror(err));
    }
  }

  ~UnixDogstatsdSink() { closeAndUnlink(); }

  UnixDogstatsdSink(const UnixDogstatsdSink&) = delete;
  UnixDogstatsdSink& operator=(const UnixDogstatsdSink&) = delete;

  UnixDogstatsdSink(UnixDogstatsdSink&& other) noexcept { *this = std::move(other); }
  UnixDogstatsdSink& operator=(UnixDogstatsdSink&& other) noexcept {
    if (this != &other) {
      closeAndUnlink();
      _fd = std::move(other._fd);
      _path = std::move(other._path);
      other._path.clear();
    }
    return *this;
  }

  [[nodiscard]] const std::string& path() const noexcept { return _path; }

  [[nodiscard]] std::string recvMessage(int timeoutMs = 200) const {
    if (!_fd) {
      return {};
    }
    pollfd pfd{};
    pfd.fd = _fd.fd();
    pfd.events = POLLIN;
    const int ready = ::poll(&pfd, 1, timeoutMs);
    if (ready <= 0 || (pfd.revents & POLLIN) == 0) {
      return {};
    }
    std::array<char, 512> buf{};
    const ssize_t bytes = ::recv(_fd.fd(), buf.data(), buf.size(), 0);
    if (bytes <= 0) {
      return {};
    }
    return {buf.data(), static_cast<std::size_t>(bytes)};
  }

  void closeAndUnlink() {
    if (!_path.empty()) {
      ::unlink(_path.c_str());
      _path.clear();
    }
  }

 private:
  static std::string makeUniquePath() {
    static std::atomic<uint64_t> counter{0};
    const auto pid = static_cast<unsigned long>(::getpid());
    const auto suffix = counter.fetch_add(1, std::memory_order_relaxed);
    return "/tmp/aeronet-dogstatsd-" + std::to_string(pid) + "-" + std::to_string(suffix);
  }

  BaseFd _fd;
  std::string _path;
};

TEST(DogStatsDTest, SendsAllMetricTypesWithTags) {
  UnixDogstatsdSink sink;
  DogStatsD client(sink.path(), "svc");
  DogStatsD::DogStatsDTags tags;
  tags.append("env:dev");
  tags.append("role:web");

  client.increment("hits", 3, tags);
  EXPECT_EQ(sink.recvMessage(), "svc.hits:3|c|#env:dev,role:web");

  client.gauge("temp", 12.5, tags);
  EXPECT_EQ(sink.recvMessage(), "svc.temp:12.5|g|#env:dev,role:web");

  client.histogram("payload", 4.25, {});
  EXPECT_EQ(sink.recvMessage(), "svc.payload:4.25|h");

  client.timing("latency", std::chrono::milliseconds{42}, {});
  EXPECT_EQ(sink.recvMessage(), "svc.latency:42|ms");

  client.set("users", "abc", tags);
  EXPECT_EQ(sink.recvMessage(), "svc.users:abc|s|#env:dev,role:web");
}

TEST(DogStatsDTest, RespectsExistingNamespaceDotAndEmptyTags) {
  UnixDogstatsdSink sink;
  DogStatsD client(sink.path(), "svc.");

  client.increment("requests");
  EXPECT_EQ(sink.recvMessage(), "svc.requests:1|c");
}

TEST(DogStatsDTest, EmptySocketPathDisablesClient) {
  DogStatsD client({}, "svc");
  EXPECT_NO_THROW(client.increment("noop"));
  EXPECT_NO_THROW(client.gauge("noop", 1.0));
  EXPECT_NO_THROW(client.histogram("noop", 2.0));
  EXPECT_NO_THROW(client.timing("noop", std::chrono::milliseconds{1}));
  EXPECT_NO_THROW(client.set("noop", "value"));
}

TEST(DogStatsDTest, RejectsTooLongSocketPath) {
  std::string veryLong(sizeof(sockaddr_un{}.sun_path), 'a');
  EXPECT_THROW(DogStatsD(veryLong, {}), std::invalid_argument);
}

TEST(DogStatsDTest, RejectsTooLongNamespace) {
  std::string ns(256, 'n');
  EXPECT_THROW(DogStatsD({}, ns), std::invalid_argument);
}

TEST(DogStatsDTest, SendFailureLogsAndContinues) {
  UnixDogstatsdSink sink;
  DogStatsD client(sink.path(), "svc");
  sink.closeAndUnlink();
  EXPECT_NO_THROW(client.increment("lost", 1));
}

}  // namespace
}  // namespace aeronet
