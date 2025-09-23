#pragma once

namespace aeronet {

// Simple RAII class wrapping a socket file descriptor.
class BaseFd {
 public:
  BaseFd(const BaseFd &) = delete;
  BaseFd(BaseFd &&other) noexcept;
  BaseFd &operator=(const BaseFd &) = delete;
  BaseFd &operator=(BaseFd &&other) noexcept;

  ~BaseFd();

  [[nodiscard]] int fd() const { return _fd; }

  [[nodiscard]] bool isOpened() const { return _fd != -1; }

  // Close the underlying file descriptor immediately.
  // Typically you should rely on RAII (destructor) except when you need to:
  //  * perform an early shutdown before object lifetime ends (e.g. HttpServer::stop())
  //  * observe/force close errors deterministically at a specific point
  // Idempotent: multiple calls after first successful/failed close are no-ops.
  int close() noexcept;

  bool operator==(const BaseFd &) const noexcept = default;

  operator int() const noexcept { return _fd; }

 protected:
  BaseFd() noexcept = default;

  explicit BaseFd(int fd) noexcept : _fd(fd) {}

 private:
  int _fd = -1;
};

}  // namespace aeronet
