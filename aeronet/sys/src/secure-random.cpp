#include "aeronet/secure-random.hpp"

#include <cstddef>
#include <span>

#ifdef AERONET_LINUX
#include <sys/random.h>
#include <sys/types.h>

#include <cerrno>

#include "aeronet/errno-throw.hpp"
#elifdef AERONET_WINDOWS
// clang-format off
#include <windows.h>  // before bcrypt.h, which uses its types
#include <bcrypt.h>
// clang-format on

#include <algorithm>
#include <format>
#include <limits>
#include <system_error>
#else
#include <cstdlib>  // arc4random_buf
#endif

namespace aeronet {

void FillSecureRandomBytes(std::span<std::byte> out) {
#ifdef AERONET_LINUX
  while (!out.empty()) {
    // At most 256 bytes are always returned whole once the generator is initialized, larger requests may be short.
    const ssize_t nbBytes = ::getrandom(out.data(), out.size(), 0);
    if (nbBytes < 0) {
      if (errno == EINTR) {
        continue;
      }
      const std::size_t nbMissingBytes = out.size();
      ThrowSystemError("getrandom() failed for {} bytes", nbMissingBytes);
    }
    out = out.subspan(static_cast<std::size_t>(nbBytes));
  }
#elifdef AERONET_WINDOWS
  // BCryptGenRandom() rejects a null buffer (that of an empty span) and takes a 32-bit size.
  while (!out.empty()) {
    const auto nbBytes = static_cast<ULONG>(std::min<std::size_t>(out.size(), std::numeric_limits<ULONG>::max()));
    const NTSTATUS status =
        ::BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(out.data()), nbBytes, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (!BCRYPT_SUCCESS(status)) {
      // An NTSTATUS is not a Win32 error code: std::system_category() cannot describe it, hence its value in the text.
      throw std::system_error(static_cast<int>(status), std::system_category(),
                              std::format("BCryptGenRandom() failed for {} bytes with NTSTATUS {:#010x}", nbBytes,
                                          static_cast<unsigned long>(status)));
    }
    out = out.subspan(nbBytes);
  }
#else
  ::arc4random_buf(out.data(), out.size());  // never fails
#endif
}

}  // namespace aeronet
