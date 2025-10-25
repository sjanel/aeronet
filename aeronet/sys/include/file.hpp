#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "base-fd.hpp"

namespace aeronet {

class File {
 public:
  enum class OpenMode : uint8_t { ReadOnly };

  // Default-constructed File is closed / empty.
  File() noexcept = default;

  // Open a file by path. Throws on error.
  // The returned File owns the underlying descriptor and will close it on destruction.
  explicit File(const std::string& path, OpenMode mode = OpenMode::ReadOnly) : File(path.c_str(), mode) {}

  // Path overloads accepting string_view or C-string for convenience.
  explicit File(std::string_view path, OpenMode mode = OpenMode::ReadOnly);
  explicit File(const char* path, OpenMode mode = OpenMode::ReadOnly);

  // Returns true when the File currently holds an open descriptor.
  explicit operator bool() const noexcept { return static_cast<bool>(_fd); }

  // Return the file size in bytes. Throws std::runtime_error on failure.
  [[nodiscard]] std::size_t size() const;

  // Load the entire file content into a string. Throws on error.
  [[nodiscard]] std::string loadAllContent() const;

 private:
  friend struct ConnectionState;

  // Returns the raw underlying file descriptor. Valid only when BaseFd is opened.
  // The caller does NOT take ownership of the descriptor; the File instance remains
  // responsible for closing it (unless you explicitly adopt the fd elsewhere first).
  [[nodiscard]] int fd() const noexcept { return _fd.fd(); }

  BaseFd _fd;
};

}  // namespace aeronet
