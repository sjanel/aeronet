#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>

#include "aeronet/base-fd.hpp"
#include "aeronet/mime-mappings.hpp"

namespace aeronet {

class File {
 public:
  enum class OpenMode : uint8_t { ReadOnly };

  static constexpr std::size_t kError = std::numeric_limits<std::size_t>::max();

  // Default-constructed File is closed / empty.
  File() noexcept = default;

  // Open a file by path. Throws on error.
  // On success, the returned File owns the underlying descriptor and will close it on destruction.
  // On failure, operator bool() returns false.
  explicit File(const std::string& path, OpenMode mode = OpenMode::ReadOnly) : File(path.c_str(), mode) {}

  // Open a file by path. Throws on error.
  // On success, the returned File owns the underlying descriptor and will close it on destruction.
  // On failure, operator bool() returns false.
  explicit File(std::string_view path, OpenMode mode = OpenMode::ReadOnly);

  // Open a file by path (must be null-terminated). Throws on error.
  // On success, the returned File owns the underlying descriptor and will close it on destruction.
  // On failure, operator bool() returns false.
  explicit File(const char* path, OpenMode mode = OpenMode::ReadOnly);

  // Returns true when the File currently holds an opened descriptor.
  explicit operator bool() const noexcept { return static_cast<bool>(_fd); }

  // Return the file size in bytes, at the time of opening.
  [[nodiscard]] std::size_t size() const noexcept { return _fileSize; }

  // Read up to dst.size() bytes starting at the given absolute offset.
  // Uses pread() so it does not modify the file's current offset.
  // Returns the number of bytes read (0 on EOF). Returns kError on error.
  [[nodiscard]] std::size_t readAt(std::span<std::byte> dst, std::size_t offset) const;

  // Returns the probable content type based on the file extension.
  // If not found, return 'application/octet-stream'.
  [[nodiscard]] std::string_view detectedContentType() const;

 private:
  friend struct ConnectionState;

  // Returns the raw underlying file descriptor. Valid only when BaseFd is opened.
  // The caller does NOT take ownership of the descriptor; the File instance remains
  // responsible for closing it (unless you explicitly adopt the fd elsewhere first).
  [[nodiscard]] int fd() const noexcept { return _fd.fd(); }

  BaseFd _fd;
  MIMETypeIdx _mimeMappingIdx = kUnknownMIMEMappingIdx;
  std::size_t _fileSize{kError};
};

}  // namespace aeronet
