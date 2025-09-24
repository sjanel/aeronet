// Simple RAII temporary file helper for tests (modern C++ implementation).
#pragma once

#include <filesystem>
#include <fstream>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

class TempFile {
 public:
  static TempFile createWithContent(std::string_view prefix, std::string_view content) {
    namespace fs = std::filesystem;
    const fs::path base = fs::temp_directory_path();
    // Use random device + time for uniqueness (sufficient for tests; mkstemp-level guarantees not required here).
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<unsigned long long> dist;  // NOLINT(runtime/int)
    for (int attempt = 0; attempt < 16; ++attempt) {
      auto token = dist(gen);
      fs::path candidate = base / (std::string(prefix) + toHex(token) + ".tmp");
      std::error_code ecExists;
      if (fs::exists(candidate, ecExists)) {
        continue;  // collision; unlikely
      }
      std::ofstream ofs(candidate, std::ios::binary | std::ios::out | std::ios::trunc);
      if (!ofs) {
        continue;  // try next
      }
      if (!content.empty()) {
        ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!ofs) {
          // Failed write; cleanup and retry
          ofs.close();
          std::error_code ec;
          fs::remove(candidate, ec);
          continue;
        }
      }
      ofs.close();
      return TempFile(candidate.string());
    }
    throw std::runtime_error("TempFile: unable to create unique file after attempts");
  }

  TempFile() = default;

  explicit TempFile(std::string path) : _path(std::move(path)) {}

  TempFile(const TempFile&) = delete;
  TempFile& operator=(const TempFile&) = delete;
  TempFile(TempFile&& other) noexcept : _path(std::move(other._path)) { other._path.clear(); }
  TempFile& operator=(TempFile&& other) noexcept {
    if (this != &other) {
      removeNow();
      _path = std::move(other._path);
      other._path.clear();
    }
    return *this;
  }

  ~TempFile() { removeNow(); }

  [[nodiscard]] std::string_view path() const { return _path; }
  [[nodiscard]] bool valid() const { return !_path.empty(); }

 private:
  static std::string toHex(unsigned long long value) {  // NOLINT(runtime/int)
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(16);
    for (int i = 0; i < 16; ++i) {
      out.push_back(kHex[(value >> (i * 4)) & 0xF]);
    }
    return out;
  }
  void removeNow() noexcept {
    if (_path.empty()) {
      return;
    }
    std::error_code ec;
    std::filesystem::remove(_path, ec);  // ignore errors
    _path.clear();
  }
  std::string _path;
};
