#include "aeronet/log-capture.hpp"

#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <string>

#include "aeronet/errno-throw.hpp"

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace aeronet::test {

namespace {

#ifdef _WIN32
int StdoutFd() { return ::_fileno(stdout); }
int FileFd(std::FILE* file) { return ::_fileno(file); }
int DupFd(int fd) { return ::_dup(fd); }
int Dup2Fd(int fd, int fd2) { return ::_dup2(fd, fd2); }
int CloseFd(int fd) { return ::_close(fd); }
#else
int StdoutFd() { return ::fileno(stdout); }
int FileFd(std::FILE* file) { return ::fileno(file); }
int DupFd(int fd) { return ::dup(fd); }
int Dup2Fd(int fd, int fd2) { return ::dup2(fd, fd2); }
int CloseFd(int fd) { return ::close(fd); }
#endif

}  // namespace

LogCapture::LogCapture() {
  // Appending: once the file is emptied by take(), the next writes go to its start.
  std::FILE* file = std::fopen(_path.string().c_str(), "ab");
  if (file == nullptr) {
    ThrowSystemError(errno, "LogCapture: cannot create {}", _path.string());
  }
  std::fflush(stdout);  // what was written before goes to the real stdout
  _savedStdout = DupFd(StdoutFd());
  const int err = _savedStdout == -1 || Dup2Fd(FileFd(file), StdoutFd()) == -1 ? errno : 0;
  std::fclose(file);  // stdout keeps the file open
  if (err != 0) {
    if (_savedStdout != -1) {
      CloseFd(_savedStdout);
    }
    ThrowSystemError(err, "LogCapture: cannot redirect stdout");
  }
}

LogCapture::~LogCapture() {
  std::fflush(stdout);
  [[maybe_unused]] const bool restored = Dup2Fd(_savedStdout, StdoutFd()) != -1 && CloseFd(_savedStdout) == 0;
  assert(restored);
  const std::string rest = take();
  [[maybe_unused]] const std::size_t written = std::fwrite(rest.data(), 1, rest.size(), stdout);
  assert(written == rest.size());
}

std::string LogCapture::take() {
  std::fflush(stdout);
  std::string captured;
  {
    // A stream of its own: reading through the descriptor of stdout would move its file offset.
    std::ifstream in(_path, std::ios::binary);
    captured.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  std::filesystem::resize_file(_path, 0);
  return captured;
}

}  // namespace aeronet::test
