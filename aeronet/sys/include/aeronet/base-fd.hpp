#pragma once

namespace aeronet {

// Simple RAII class wrapping a socket file descriptor.
class BaseFd {
 public:
  static constexpr int kClosedFd = -1;

  explicit BaseFd(int fd = kClosedFd) noexcept : _fd(fd) {}

  BaseFd(const BaseFd& other) = delete;
  BaseFd(BaseFd&& other) noexcept : _fd(other.release()) {}
  BaseFd& operator=(const BaseFd& other) = delete;
  BaseFd& operator=(BaseFd&& other) noexcept;

  ~BaseFd() { close(); }

  [[nodiscard]] int fd() const noexcept { return _fd; }

  // Truthy check so users can write: if (baseFd) { ... }
  // Returns true if the underlying fd is valid (not closed).
  explicit operator bool() const noexcept { return _fd != kClosedFd; }

  // Release ownership of the underlying fd without closing it.
  // Returns the raw fd and sets this object to closed state.
  [[nodiscard]] int release() noexcept;

  // Close the underlying file descriptor immediately.
  // Typically you should rely on RAII (destructor) except when you need to:
  //  * perform an early shutdown before object lifetime ends (e.g. SingleHttpServer::stop())
  //  * observe/force close errors deterministically at a specific point
  // Idempotent: multiple calls after first successful/failed close are no-ops.
  void close() noexcept;

  // Equality comparison - simply compare the underlying fd integer.
  bool operator==(const BaseFd&) const noexcept = default;

 private:
  int _fd;
};

}  // namespace aeronet
