#pragma once

namespace aeronet {

// Simple RAII class wrapping a socket file descriptor.
class BaseFd {
 public:
  static constexpr int kClosedFd = -1;

  BaseFd() noexcept = default;

  explicit BaseFd(int fd) noexcept : _fd(fd) {}

  BaseFd(const BaseFd &) = delete;
  BaseFd(BaseFd &&other) noexcept;
  BaseFd &operator=(const BaseFd &) = delete;
  BaseFd &operator=(BaseFd &&other) noexcept;

  ~BaseFd();

  [[nodiscard]] int fd() const noexcept { return _fd; }

  [[nodiscard]] bool isOpened() const noexcept { return _fd != kClosedFd; }

  // Close the underlying file descriptor immediately.
  // Typically you should rely on RAII (destructor) except when you need to:
  //  * perform an early shutdown before object lifetime ends (e.g. HttpServer::stop())
  //  * observe/force close errors deterministically at a specific point
  // Idempotent: multiple calls after first successful/failed close are no-ops.
  void close() noexcept;

  bool operator==(const BaseFd &) const noexcept = default;

 private:
  int _fd = kClosedFd;
};

}  // namespace aeronet
