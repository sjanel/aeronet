#include "aeronet/errno-throw.hpp"

#include <gtest/gtest.h>

#include <string>
#include <system_error>

#ifndef AERONET_WINDOWS
#include <cerrno>
#endif

namespace aeronet {

#ifdef AERONET_WINDOWS
TEST(ErrnoThrow, ThrowsSystemErrorWithWSAError) {
  try {
    WSASetLastError(WSAECONNREFUSED);
    int fd = 42;
    ThrowSystemError("Test error with code {}", fd);
  } catch (const std::system_error& e) {
    EXPECT_EQ(e.code().value(), WSAECONNREFUSED);
    EXPECT_EQ(e.code().category(), std::system_category());
    // Don't check exact message string — locale-dependent on Windows
    EXPECT_NE(std::string(e.what()).find("Test error with code 42"), std::string::npos);
  } catch (...) {
    FAIL() << "Expected std::system_error, but caught different exception";
  }
}
#else
TEST(ErrnoThrow, ThrowsSystemErrorWithErrno) {
  try {
    // Simulate a system call failure by setting errno
    errno = ENOENT;  // No such file or directory
    int fd = 42;
    ThrowSystemError("Test error with code {}", fd);
    FAIL() << "Expected std::system_error to be thrown";
  } catch (const std::system_error& e) {
    EXPECT_EQ(e.code().value(), ENOENT);
    EXPECT_EQ(e.code().category(), std::generic_category());
    EXPECT_EQ(e.what(), std::string("Test error with code 42: No such file or directory"));
  } catch (...) {
    FAIL() << "Expected std::system_error, but caught different exception";
  }
}
#endif

}  // namespace aeronet