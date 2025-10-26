#include "aeronet/temp-file.hpp"

#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include "base-fd.hpp"
#include "log.hpp"

namespace aeronet::test {

namespace {
std::string toHex(uint64_t value) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(16);
  for (int i = 15; i >= 0; --i) {
    out.push_back(kHex[(value >> (i * 4)) & 0xF]);
  }
  return out;
}

std::mt19937_64 &threadRng() {
  static std::mt19937_64 engine = [] {
    // Collect multiple entropy sources and mix via seed_seq.
    std::random_device rd;
    const auto now = static_cast<uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    const auto tid = static_cast<uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    const auto addr = reinterpret_cast<uint64_t>(&rd);
    std::array<uint64_t, 4> seeds{static_cast<uint64_t>(rd()), now, tid, addr};
    std::seed_seq seq(seeds.begin(), seeds.end());
    return std::mt19937_64(seq);
  }();
  return engine;
}

// Note: ScopedTempFile should not create directories. The helper used to
// create a unique directory was removed in favor of having callers provide
// a ScopedTempDir. File creation is performed by the ScopedTempFile
// constructor below.

}  // namespace

ScopedTempDir::ScopedTempDir(std::string_view prefix) {
  const auto base = std::filesystem::temp_directory_path();
  std::uniform_int_distribution<uint64_t> dist;
  for (int attempt = 0; attempt < 100; ++attempt) {
    const auto candidate = base / (std::string(prefix) + toHex(dist(threadRng())));
    std::error_code ec;
    if (std::filesystem::create_directories(candidate, ec)) {
      _dir = candidate;
      return;
    }
  }

  throw std::runtime_error("ScopedTempDir: Failed to create temp dir");
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

// Create a file inside the provided ScopedTempDir. The constructor does not
// create directories; callers must ensure the target directory exists.
ScopedTempFile::ScopedTempFile(const ScopedTempDir &dir, std::string_view content) {
  _dir = dir.dirPath();

  // Create a unique file inside the provided directory. Use mkstemp on a
  // template so we get an atomic create+open and avoid races.
  std::string tmpl = _dir.string() + "/aeronet_temp_XXXXXX";

  BaseFd raii(::mkstemp(tmpl.data()));
  int fd = raii.fd();
  if (fd == -1) {
    int err = errno;
    throw std::system_error(err, std::generic_category(), "ScopedTempFile: mkstemp failed");
  }

  // mkstemp returns the path it created in buf.data()
  _path = std::filesystem::path(tmpl.data());

  auto written = ::write(fd, content.data(), content.size());
  if (std::cmp_not_equal(written, content.size())) {
    // best-effort cleanup: try to unlink the file we just created
    int rc = ::unlink(_path.c_str());
    if (rc != 0) {
      int err = errno;
      log::error("ScopedTempFile: unlink({}) failed: {} ({})", _path.string(), err, std::strerror(err));
    }
    throw std::runtime_error("ScopedTempFile: write failed");
  }

  _content.assign(content);
}

ScopedTempFile::ScopedTempFile(ScopedTempFile &&other) noexcept
    : _dir(std::move(other._dir)), _path(std::move(other._path)), _content(std::move(other._content)) {
  other._path.clear();
}

ScopedTempFile &ScopedTempFile::operator=(ScopedTempFile &&other) noexcept {
  if (this != &other) {
    cleanup();
    _dir = std::move(other._dir);
    _path = std::move(other._path);
    _content = std::move(other._content);

    other._path.clear();
  }
  return *this;
}

ScopedTempFile::~ScopedTempFile() { cleanup(); }

void ScopedTempFile::cleanup() noexcept {
  // Remove only the file we created. Do not touch directories — ScopedTempDir
  // is responsible for removing its directory contents.
  if (!_path.empty()) {
    std::error_code ec;
    bool removed = std::filesystem::remove(_path, ec);
    if (ec) {
      log::error("ScopedTempFile::cleanup: remove({}) failed: {} ({})", _path.string(), ec.value(), ec.message());
    } else if (!removed) {
      log::error("ScopedTempFile::cleanup: expected to remove file {}, but nothing was removed", _path.string());
    }
  }
}

}  // namespace aeronet::test