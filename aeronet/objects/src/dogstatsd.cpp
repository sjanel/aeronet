#include "aeronet/dogstatsd.hpp"

#include <sys/socket.h>
#include <sys/un.h>

#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <string_view>
#include <thread>

#include "aeronet/base-fd.hpp"
#include "aeronet/errno-throw.hpp"
#include "aeronet/log.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/stringconv.hpp"

namespace aeronet {
namespace {
constexpr std::string_view kTagsPrefix{"|#"};
constexpr std::string_view kCounterSuffix{"|c"};
constexpr std::string_view kGaugeSuffix{"|g"};
constexpr std::string_view kHistogramSuffix{"|h"};
constexpr std::string_view kTimingSuffix{"|ms"};
constexpr std::string_view kSetSuffix{"|s"};
constexpr std::size_t kFloatingBufferSize = std::numeric_limits<double>::max_digits10 + 12;

std::string_view FormatFloating(double value, std::array<char, kFloatingBufferSize>& buffer) {
  const auto [ptr, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, std::chars_format::general);
  return {buffer.data(), ptr};
}
}  // namespace

DogStatsD::DogStatsD(std::string_view socketPath, std::string_view ns, std::chrono::milliseconds connectTimeout) {
  if (socketPath.size() >= sizeof(sockaddr_un{}.sun_path)) {
    throw std::invalid_argument("DogStatsD: socket path too long");
  }
  if (ns.size() >= 256UL) {
    throw std::invalid_argument("DogStatsD: namespace too long");
  }

  if (socketPath.empty()) {
    return;
  }

  uint32_t dotAppendSz = !ns.empty() && ns.back() != '.' ? 1U : 0U;

  _ns.reserve(ns.size() + dotAppendSz);
  _ns.unchecked_append(ns);
  if (dotAppendSz != 0) {
    _ns.unchecked_push_back('.');
  }

  _fd = BaseFd(::socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
  if (!_fd) {
    throw_errno("DogStatsD: socket creation failed");
  }

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::memcpy(addr.sun_path, socketPath.data(), socketPath.size());
  addr.sun_path[socketPath.size()] = '\0';
  auto addrlen = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + socketPath.size() + 1);

  bool connected = false;
  const auto stepWait = connectTimeout / 10;
  for (const auto deadline = std::chrono::steady_clock::now() + connectTimeout;
       std::chrono::steady_clock::now() < deadline; std::this_thread::sleep_for(stepWait)) {
    if (::connect(_fd.fd(), reinterpret_cast<sockaddr*>(&addr), addrlen) == 0) {
      connected = true;
      break;
    }
    log::debug("DogStatsD: connect failed for fd # {}: {}", _fd.fd(), std::strerror(errno));
  }
  if (!connected) {
    throw std::runtime_error("DogStatsD: connect timeout");
  }
}

void DogStatsD::sendMetricMessage(std::string_view metric, std::string_view value, std::string_view typeSuffix,
                                  const DogStatsDTags& tags) const noexcept {
  if (!_fd) {
    return;
  }

  const auto tagsSize = tags.empty() ? 0UL : kTagsPrefix.size() + tags.fullSize();

  try {
    RawChars msg(_ns.size() + metric.size() + 1U + value.size() + typeSuffix.size() + tagsSize);

    msg.unchecked_append(_ns);
    msg.unchecked_append(metric);
    msg.unchecked_push_back(':');
    msg.unchecked_append(value);
    msg.unchecked_append(typeSuffix);
    if (tagsSize != 0) {
      msg.unchecked_append(kTagsPrefix);
      msg.unchecked_append(tags.fullString());
    }

    if (::send(_fd.fd(), msg.data(), msg.size(), MSG_DONTWAIT | MSG_NOSIGNAL) == -1) {
      log::error("DogStatsD: unable to send message of size {} with error : {}", msg.size(), std::strerror(errno));
    }
  } catch (const std::bad_alloc&) {
    log::error("DogStatsD: unable to allocate memory for metric message");
    return;
  }
}

void DogStatsD::increment(std::string_view metric, uint64_t value, const DogStatsDTags& tags) const noexcept {
  sendMetricMessage(metric, std::string_view(IntegralToCharVector(value)), kCounterSuffix, tags);
}

void DogStatsD::gauge(std::string_view metric, int64_t value, const DogStatsDTags& tags) const noexcept {
  sendMetricMessage(metric, std::string_view(IntegralToCharVector(value)), kGaugeSuffix, tags);
}

void DogStatsD::histogram(std::string_view metric, double value, const DogStatsDTags& tags) const noexcept {
  std::array<char, kFloatingBufferSize> buffer{};
  sendMetricMessage(metric, FormatFloating(value, buffer), kHistogramSuffix, tags);
}

void DogStatsD::timing(std::string_view metric, std::chrono::milliseconds ms,
                       const DogStatsDTags& tags) const noexcept {
  sendMetricMessage(metric, std::string_view(IntegralToCharVector(ms.count())), kTimingSuffix, tags);
}

void DogStatsD::set(std::string_view metric, std::string_view value, const DogStatsDTags& tags) const noexcept {
  sendMetricMessage(metric, value, kSetSuffix, tags);
}

}  // namespace aeronet
