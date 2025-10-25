#include "aeronet/temp-file.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <ios>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace aeronet::test {

// ScopedTempDir implementation
ScopedTempDir::ScopedTempDir(std::string_view prefix) {
  const auto base = std::filesystem::temp_directory_path();
  for (int i = 0; i < 1000; ++i) {
    _dir = base / (std::string(prefix) +
                   std::to_string(static_cast<std::uint64_t>(std::hash<std::string>{}(std::to_string(i)))));
    if (!std::filesystem::exists(_dir)) {
      std::error_code ec;
      std::filesystem::create_directories(_dir, ec);
      if (!ec) {
        break;
      }
    }
    _dir.clear();
  }
  if (_dir.empty()) {
    throw std::runtime_error("Failed to create temp dir for ScopedTempDir");
  }
}

ScopedTempDir::ScopedTempDir(ScopedTempDir &&other) noexcept : _dir(std::move(other._dir)) { other._dir.clear(); }

ScopedTempDir &ScopedTempDir::operator=(ScopedTempDir &&other) noexcept {
  if (this != &other) {
    cleanup();
    _dir = std::move(other._dir);
    other._dir.clear();
  }
  return *this;
}

ScopedTempDir::~ScopedTempDir() { cleanup(); }

void ScopedTempDir::cleanup() noexcept {
  if (!_dir.empty()) {
    std::error_code ec;
    std::filesystem::remove_all(_dir, ec);
    _dir.clear();
  }
}

namespace {
std::string toHex(unsigned long long value) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(16);
  for (int i = 15; i >= 0; --i) {
    out.push_back(kHex[(value >> (i * 4)) & 0xF]);
  }
  return out;
}
}  // namespace

// Create a uniquely-named temp file using the provided prefix and content.
// This mirrors the small helper used in some tests.
ScopedTempFile ScopedTempFile::create(std::string_view prefix, std::string_view content) {
  std::random_device rd;
  std::mt19937_64 gen(rd());
  std::uniform_int_distribution<unsigned long long> dist;
  for (int attempt = 0; attempt < 64; ++attempt) {
    const auto name = std::string(prefix) + toHex(dist(gen));
    return {name, content};
  }
  throw std::runtime_error("ScopedTempFile::create: unable to create unique file");
}

// ScopedTempFile implementation
ScopedTempFile::ScopedTempFile(std::string_view name, std::string_view content) {
  const auto base = std::filesystem::temp_directory_path();
  for (int i = 0; i < 1000; ++i) {
    _dir = base / ("aeronet-temp-file-" +
                   std::to_string(static_cast<std::uint64_t>(std::hash<std::string>{}(std::to_string(i)))));
    if (!std::filesystem::exists(_dir)) {
      std::error_code ec;
      std::filesystem::create_directories(_dir, ec);
      if (!ec) {
        break;
      }
    }
    _dir.clear();
  }
  if (_dir.empty()) {
    throw std::runtime_error("Failed to create temp dir for ScopedTempFile");
  }
  _path = _dir / std::string(name);
  std::ofstream out(_path, std::ios::binary);
  if (!out) {
    cleanup();
    throw std::runtime_error("ScopedTempFile: failed to open file for writing");
  }
  out.write(content.data(), static_cast<std::streamsize>(content.size()));
  out.close();
  _content.assign(content);
}

ScopedTempFile::ScopedTempFile(const ScopedTempDir &dir, std::string_view name, std::string_view content) {
  if (dir.dirPath().empty()) {
    throw std::runtime_error("ScopedTempFile: provided dir is empty");
  }
  _dir = dir.dirPath();
  _path = _dir / std::string(name);
  std::ofstream out(_path, std::ios::binary);
  if (!out) {
    cleanup();
    throw std::runtime_error("ScopedTempFile: failed to open file for writing");
  }
  out.write(content.data(), static_cast<std::streamsize>(content.size()));
  out.close();
  _content.assign(content);
}

ScopedTempFile::ScopedTempFile(std::string_view name, std::uint64_t size) {
  const auto base = std::filesystem::temp_directory_path();
  for (int i = 0; i < 1000; ++i) {
    _dir = base / ("aeronet-temp-file-" +
                   std::to_string(static_cast<std::uint64_t>(std::hash<std::string>{}(std::to_string(i)))));
    if (!std::filesystem::exists(_dir)) {
      std::error_code ec;
      std::filesystem::create_directories(_dir, ec);
      if (!ec) {
        break;
      }
    }
    _dir.clear();
  }
  if (_dir.empty()) {
    throw std::runtime_error("Failed to create temp dir for ScopedTempFile");
  }
  _path = _dir / std::string(name);
  _content.assign(static_cast<size_t>(size), '\0');
  for (std::uint64_t i = 0; i < size; ++i) {
    _content[static_cast<size_t>(i)] = static_cast<char>('a' + (i % 26));
  }
  std::ofstream out(_path, std::ios::binary);
  if (!out) {
    cleanup();
    throw std::runtime_error("ScopedTempFile: failed to open file for writing");
  }
  out.write(_content.data(), static_cast<std::streamsize>(_content.size()));
  out.close();
}

ScopedTempFile::ScopedTempFile(const ScopedTempDir &dir, std::string_view name, std::uint64_t size) {
  if (dir.dirPath().empty()) {
    throw std::runtime_error("ScopedTempFile: provided dir is empty");
  }
  _dir = dir.dirPath();
  _path = _dir / std::string(name);
  _content.assign(static_cast<size_t>(size), '\0');
  for (std::uint64_t i = 0; i < size; ++i) {
    _content[static_cast<size_t>(i)] = static_cast<char>('a' + (i % 26));
  }
  std::ofstream out(_path, std::ios::binary);
  if (!out) {
    cleanup();
    throw std::runtime_error("ScopedTempFile: failed to open file for writing");
  }
  out.write(_content.data(), static_cast<std::streamsize>(_content.size()));
  out.close();
}

ScopedTempFile::ScopedTempFile(ScopedTempFile &&other) noexcept
    : _dir(std::move(other._dir)), _path(std::move(other._path)), _content(std::move(other._content)) {
  other._dir.clear();
  other._path.clear();
  other._content.clear();
}

ScopedTempFile &ScopedTempFile::operator=(ScopedTempFile &&other) noexcept {
  if (this != &other) {
    cleanup();
    _dir = std::move(other._dir);
    _path = std::move(other._path);
    _content = std::move(other._content);
    other._dir.clear();
    other._path.clear();
    other._content.clear();
  }
  return *this;
}

ScopedTempFile::~ScopedTempFile() { cleanup(); }

void ScopedTempFile::cleanup() noexcept {
  if (!_dir.empty()) {
    std::error_code ec;
    std::filesystem::remove_all(_dir, ec);
    _dir.clear();
    _path.clear();
    _content.clear();
  }
}

}  // namespace aeronet::test