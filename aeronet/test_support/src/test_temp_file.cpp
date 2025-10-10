#include "aeronet/test_temp_file.hpp"

#include <filesystem>
#include <fstream>
#include <ios>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "log.hpp"

namespace {
std::string toHex(unsigned long long value) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(16);
  for (int i = 0; i < 16; ++i) {
    out.push_back(kHex[(value >> (i * 4)) & 0xF]);
  }
  return out;
}
}  // namespace

TempFile TempFile::createWithContent(std::string_view prefix, std::string_view content) {
  namespace fs = std::filesystem;
  const fs::path base = fs::temp_directory_path();
  // Use random device + time for uniqueness (sufficient for tests; mkstemp-level guarantees not required here).
  std::random_device rd;
  std::mt19937_64 gen(rd());
  std::uniform_int_distribution<unsigned long long> dist;
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

TempFile::~TempFile() { removeNow(); }

TempFile::TempFile(TempFile&& other) noexcept : _path(std::move(other._path)) { other._path.clear(); }

TempFile& TempFile::operator=(TempFile&& other) noexcept {
  if (this != &other) {
    removeNow();
    _path = std::move(other._path);
    other._path.clear();
  }
  return *this;
}

void TempFile::removeNow() noexcept {
  if (_path.empty()) {
    return;
  }
  std::error_code ec;
  if (!std::filesystem::remove(_path, ec)) {
    aeronet::log::error("TempFile: unable to remove {}: {}", _path, ec.message());
  }
  _path.clear();
}