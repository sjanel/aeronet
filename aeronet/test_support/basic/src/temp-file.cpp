#include "aeronet/temp-file.hpp"

#include "aeronet/errno-throw.hpp"
#include "aeronet/system-error-message.hpp"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#define AERONET_WRITE _write
#define AERONET_UNLINK _unlink
#else
#include <fcntl.h>
#include <unistd.h>
#define AERONET_WRITE ::write
#define AERONET_UNLINK ::unlink
#endif

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include "aeronet/base-fd.hpp"
#include "aeronet/log.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/system-error.hpp"

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

std::mt19937_64& threadRng() {
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

ScopedTempDir::ScopedTempDir(ScopedTempDir&& other) noexcept : _dir(std::move(other._dir)) { other._dir.clear(); }

ScopedTempDir& ScopedTempDir::operator=(ScopedTempDir&& other) noexcept {
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

ScopedTempFile::ScopedTempFile(const ScopedTempDir& dir, std::string_view content) {
  _dir = dir.dirPath();

  std::uniform_int_distribution<uint64_t> dist;
  std::string tmpl;
  NativeHandle fd = kInvalidHandle;

  for (int attempt = 0; attempt < 100; ++attempt) {
    tmpl = _dir.string() + "/aeronet_temp_" + toHex(dist(threadRng()));
#ifdef _WIN32
    const int crtFd = _open(tmpl.c_str(), _O_CREAT | _O_EXCL | _O_RDWR | _O_BINARY, _S_IREAD | _S_IWRITE);
    fd = static_cast<NativeHandle>(crtFd);
#else
    fd = ::open(tmpl.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
#endif
    if (fd != kInvalidHandle) {
      break;
    }
  }

#ifdef _WIN32
  BaseFd raii(fd, BaseFd::HandleKind::CrtFd);
#else
  BaseFd raii(fd);
#endif

  if (fd == -1) {
    ThrowSystemError("ScopedTempFile: open failed");
  }

  _path = std::filesystem::path(tmpl);

  const auto written = AERONET_WRITE(static_cast<int>(fd), content.data(), static_cast<unsigned int>(content.size()));
  if (written < 0 || static_cast<std::size_t>(written) != content.size()) {
    // best-effort cleanup: try to unlink the file we just created
    const auto rc = AERONET_UNLINK(_path.string().c_str());
    if (rc != 0) {
      int err = LastSystemError();
      log::error("ScopedTempFile: unlink({}) failed: {} ({})", _path.string(), err, SystemErrorMessage(err));
    }
    throw std::runtime_error("ScopedTempFile: write failed");
  }

  _content.assign(content);
}

ScopedTempFile::ScopedTempFile(ScopedTempFile&& other) noexcept
    : _dir(std::move(other._dir)), _path(std::move(other._path)), _content(std::move(other._content)) {
  other._path.clear();
}

ScopedTempFile& ScopedTempFile::operator=(ScopedTempFile&& other) noexcept {
  if (this != &other) {
    cleanup();
    _dir = std::move(other._dir);
    _path = std::move(other._path);
    _content = std::move(other._content);

    other._path.clear();
  }
  return *this;
}

void ScopedTempFile::cleanup() noexcept {
  // Remove only the file we created.
  if (!_path.empty()) {
    std::error_code ec;
    bool removed = std::filesystem::remove(_path, ec);
    if (ec) {
      log::error("ScopedTempFile::cleanup: remove({}) failed: {} ({})", _path.string(), ec.value(), ec.message());
    } else if (!removed) {
      log::error("ScopedTempFile::cleanup: expected to remove file {}, but nothing was removed", _path.string());
    }
    _path.clear();
  }
}
}  // namespace aeronet::test
