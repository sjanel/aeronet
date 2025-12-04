#pragma once

#include <cstring>
#include <string>
#include <utility>

#include "aeronet/base-fd.hpp"

namespace aeronet::test {

class UnixDogstatsdSink {
 public:
  UnixDogstatsdSink();

  ~UnixDogstatsdSink() { closeAndUnlink(); }

  UnixDogstatsdSink(const UnixDogstatsdSink&) = delete;
  UnixDogstatsdSink& operator=(const UnixDogstatsdSink&) = delete;

  UnixDogstatsdSink(UnixDogstatsdSink&& other) noexcept : _fd(std::move(other._fd)), _path(std::move(other._path)) {}
  UnixDogstatsdSink& operator=(UnixDogstatsdSink&& other) noexcept;

  [[nodiscard]] const std::string& path() const noexcept { return _path; }

  [[nodiscard]] std::string recvMessage(int timeoutMs = 200) const;

  void closeAndUnlink();

 private:
  BaseFd _fd;
  std::string _path;
};

}  // namespace aeronet::test