#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace aeronet::test {

// ScopedTempDir: creates a unique temporary directory under the system temp
// directory and removes it on destruction. Useful to contain multiple temp files
// for tests.
class ScopedTempDir {
 public:
  // Create a uniquely-named temporary directory with optional prefix.
  explicit ScopedTempDir(std::string_view prefix = "aeronet-temp-dir-");

  ScopedTempDir(const ScopedTempDir&) = delete;
  ScopedTempDir& operator=(const ScopedTempDir&) = delete;
  ScopedTempDir(ScopedTempDir&& other) noexcept;
  ScopedTempDir& operator=(ScopedTempDir&& other) noexcept;

  ~ScopedTempDir();

  [[nodiscard]] const std::filesystem::path& dirPath() const noexcept { return _dir; }

 private:
  void cleanup() noexcept;

  std::filesystem::path _dir;
};

// ScopedTempFile: creates a unique temporary directory under the system temp
// directory and places one file inside it. The directory (and file) are
// removed when the object is destroyed. Useful for tests that need a
// filesystem path to pass to components that serve files from a directory.
class ScopedTempFile {
 public:
  // Construct a temp file inside an existing ScopedTempDir. The file will be
  // created directly under `dir.dirPath()` with a unique generated name.
  // ScopedTempFile will NOT create any directories; callers must supply an
  // existing `ScopedTempDir`. The provided directory is not modified.
  ScopedTempFile(const ScopedTempDir& dir, std::string_view content);

  ScopedTempFile(ScopedTempDir&& dir, std::string_view content) = delete;

  // Create a uniquely-named temp file using the provided prefix and content.
  // The returned ScopedTempFile will remove the file (and its containing
  // temporary directory) on destruction.

  ScopedTempFile(const ScopedTempFile&) = delete;
  ScopedTempFile& operator=(const ScopedTempFile&) = delete;
  ScopedTempFile(ScopedTempFile&& other) noexcept;
  ScopedTempFile& operator=(ScopedTempFile&& other) noexcept;

  ~ScopedTempFile() { cleanup(); }

  // Directory containing the file
  [[nodiscard]] const std::filesystem::path& dirPath() const noexcept { return _dir; }
  // Full path to the file
  [[nodiscard]] const std::filesystem::path& filePath() const noexcept { return _path; }
  // Filename only
  [[nodiscard]] std::string filename() const { return _path.filename().string(); }
  // If constructed with the size overload, returns the generated content.
  [[nodiscard]] std::string_view content() const noexcept { return _content; }

  void cleanup() noexcept;

 private:
  std::filesystem::path _dir;
  std::filesystem::path _path;
  std::string _content;
};

}  // namespace aeronet::test