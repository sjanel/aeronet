#include <openssl/ssl.h>
#include <openssl/types.h>

#include <atomic>

// These counters are used by tests in http-tls-handshake_test.cpp.
std::atomic<int> g_aeronetTestFailNextSslNew{0};    // NOLINT(misc-use-internal-linkage)
std::atomic<int> g_aeronetTestFailNextSslSetFd{0};  // NOLINT(misc-use-internal-linkage)

extern "C" SSL* AeronetSslNew(SSL_CTX* ctx) {
  int remaining = g_aeronetTestFailNextSslNew.load(std::memory_order_relaxed);
  while (remaining > 0) {
    if (g_aeronetTestFailNextSslNew.compare_exchange_weak(remaining, remaining - 1, std::memory_order_relaxed)) {
      return nullptr;
    }
  }
  return ::SSL_new(ctx);
}

extern "C" int AeronetSslSetFd(SSL* ssl, int fd) {
  int remaining = g_aeronetTestFailNextSslSetFd.load(std::memory_order_relaxed);
  while (remaining > 0) {
    if (g_aeronetTestFailNextSslSetFd.compare_exchange_weak(remaining, remaining - 1, std::memory_order_relaxed)) {
      return 0;
    }
  }
  return ::SSL_set_fd(ssl, fd);
}
