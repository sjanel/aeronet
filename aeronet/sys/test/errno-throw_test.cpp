#include "aeronet/errno-throw.hpp"

#include <gtest/gtest.h>

#include <cerrno>
#include <string>
#include <system_error>

namespace aeronet {

TEST(ErrnoThrow, ThrowsSystemErrorWithErrno) {
  try {
    // Simulate a system call failure by setting errno
    errno = ENOENT;  // No such file or directory
    int fd = 42;
    throw_errno("Test error with code {}", fd);
    FAIL() << "Expected std::system_error to be thrown";
  } catch (const std::system_error& e) {
    EXPECT_EQ(e.code().value(), ENOENT);
    EXPECT_EQ(e.code().category(), std::generic_category());
    EXPECT_EQ(e.what(), std::string("Test error with code 42: No such file or directory"));
  } catch (...) {
    FAIL() << "Expected std::system_error, but caught different exception";
  }
}

}  // namespace aeronet