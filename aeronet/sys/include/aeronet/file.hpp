#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>

#include "aeronet/base-fd.hpp"
#include "aeronet/mime-mappings.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/timedef.hpp"

namespace aeronet {

class File {
 public:
  enum class OpenMode : uint8_t { ReadOnly };

  static constexpr std::size_t kError = std::numeric_limits<std::size_t>::max();

  struct Identity {
    [[nodiscard]] const std::byte* data() const noexcept { return reinterpret_cast<const std::byte*>(&device); }
    [[nodiscard]] static constexpr std::size_t size() noexcept { return sizeof(device) + sizeof(inode); }

    uint64_t device{};
    uint64_t inode{};
  };

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

  // Copy: duplicates the underlying descriptor (dup/_dup) so the copy owns an independent, valid handle to the
  // same underlying file. size()/lastModified()/detectedContentType() metadata is copied as-is from the source
  // (not re-queried), since it describes the same file at the same open-time snapshot.
  // If duplication fails, the resulting File behaves like a failed open: operator bool() is false, size() is
  // kError, lastModified() is the invalid sentinel.
  File(const File& rhs);
  File& operator=(const File& rhs);

  // Declaring the copy ops above suppresses implicit move generation, so restore it explicitly: moving just
  // steals the BaseFd (no dup()) and copies the trivial metadata fields.
  File(File&&) noexcept = default;
  File& operator=(File&&) noexcept = default;

  ~File() = default;

  // Returns true when the File currently holds an opened descriptor.
  explicit operator bool() const noexcept { return static_cast<bool>(_fd); }

  // Return the file size in bytes, at the time of opening.
  [[nodiscard]] std::size_t size() const noexcept { return _fileSize; }

  // Return the last modification time captured from the same fstat() performed at open time.
  // Returns kInvalidTimePoint if the metadata could not be obtained (in which case the File is also closed).
  [[nodiscard]] SysTimePoint lastModified() const noexcept { return _mtime; }

  // Return the file's current descriptor identity and metadata. An invalid result means fstat failed.
  [[nodiscard]] Identity identity() const noexcept { return _identity; }

  // Read up to dst.size() bytes starting at the given absolute offset.
  // Uses pread() so it does not modify the file's current offset.
  // Returns the number of bytes read (0 on EOF). Returns kError on error.
  [[nodiscard]] std::size_t readAt(std::span<std::byte> dst, std::size_t offset) const;

  // Returns the probable content type based on the file extension.
  // If not found, return 'application/octet-stream'.
  [[nodiscard]] std::string_view detectedContentType() const;

 private:
  friend struct ConnectionState;
  // PlainTransport streams a file body straight to the socket via sendfile(2) and needs the raw fd.
  friend class PlainTransport;

  // Returns the raw underlying file descriptor. Valid only when BaseFd is opened.
  // The caller does NOT take ownership of the descriptor; the File instance remains
  // responsible for closing it (unless you explicitly adopt the fd elsewhere first).
  [[nodiscard]] NativeHandle fd() const noexcept { return _fd.fd(); }

  BaseFd _fd;
  MIMETypeIdx _mimeMappingIdx = kUnknownMIMEMappingIdx;
  Identity _identity;
  std::size_t _fileSize{kError};
  SysTimePoint _mtime{SysTimePoint::max()};  // == kInvalidTimePoint sentinel when metadata is unavailable
};

}  // namespace aeronet
