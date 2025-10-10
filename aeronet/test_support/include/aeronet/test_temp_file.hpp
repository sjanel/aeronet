// Simple RAII temporary file helper for tests (modern C++ implementation).
#pragma once

#include <string>
#include <string_view>
#include <utility>

class TempFile {
 public:
  static TempFile createWithContent(std::string_view prefix, std::string_view content);

  TempFile() = default;

  explicit TempFile(std::string path) : _path(std::move(path)) {}

  TempFile(const TempFile&) = delete;
  TempFile& operator=(const TempFile&) = delete;
  TempFile(TempFile&& other) noexcept;
  TempFile& operator=(TempFile&& other) noexcept;

  ~TempFile();

  [[nodiscard]] std::string_view path() const { return _path; }
  [[nodiscard]] bool valid() const { return !_path.empty(); }

 private:
  void removeNow() noexcept;

  std::string _path;
};
