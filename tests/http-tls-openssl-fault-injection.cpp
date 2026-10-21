#include <openssl/ssl.h>
#include <openssl/types.h>

#include <atomic>

// These counters are used by tests in http-tls-handshake_test.cpp.
std::atomic<int> g_aeronetTestFailNextSslNew{0};             // NOLINT(misc-use-internal-linkage)
std::atomic<int> g_aeronetTestFailNextSslSetFd{0};           // NOLINT(misc-use-internal-linkage)
std::atomic<int> g_aeronetTestFailNextSslSetAcceptState{0};  // NOLINT(misc-use-internal-linkage)

namespace {

// Consumes one of the failures still to inject, if any.
bool ShouldFail(std::atomic<int>& failNext) {
  int remaining = failNext.load(std::memory_order_relaxed);
  while (remaining > 0) {
    if (failNext.compare_exchange_weak(remaining, remaining - 1, std::memory_order_relaxed)) {
      return true;
    }
  }
  return false;
}

}  // namespace

extern "C" SSL* AeronetSslNew(SSL_CTX* ctx) {
  return ShouldFail(g_aeronetTestFailNextSslNew) ? nullptr : ::SSL_new(ctx);
}

extern "C" int AeronetSslSetFd(SSL* ssl, int fd) {
  return ShouldFail(g_aeronetTestFailNextSslSetFd) ? 0 : ::SSL_set_fd(ssl, fd);
}

extern "C" int AeronetSslSetAcceptState(SSL* ssl) {
  if (ShouldFail(g_aeronetTestFailNextSslSetAcceptState)) {
    return 0;
  }
  ::SSL_set_accept_state(ssl);
  return ::SSL_in_before(ssl);
}
