#include "aeronet/tls-handshake.hpp"

#include <dlfcn.h>
#include <gtest/gtest.h>
#include <openssl/ssl.h>

#include "aeronet/tls-config.hpp"
#include "aeronet/tls-handshake-callback.hpp"
#include "aeronet/tls-info.hpp"
#include "aeronet/tls-ktls.hpp"
#include "aeronet/tls-metrics.hpp"
#include "aeronet/tls-raii.hpp"

namespace {

int g_tls_handshake_test_mode = 0;
// Lazy-created test X509 and X509_NAME used by overrides
X509* g_test_x509 = nullptr;
X509_NAME* g_test_x509_name = nullptr;

void ensure_test_x509_created() {
  if (g_test_x509 == nullptr) {
    g_test_x509 = ::X509_new();
  }
  if (g_test_x509_name == nullptr) {
    g_test_x509_name = ::X509_NAME_new();
    // Add a commonName entry for realistic output when printing (optional)
    ::X509_NAME_add_entry_by_NID(g_test_x509_name, NID_commonName, MBSTRING_ASC,
                                 reinterpret_cast<const unsigned char*>("test"), -1, -1, 0);
  }
}
}  // namespace

// Test-only override: force OpenSSL to report no cipher name by returning
// nullptr from SSL_CIPHER_get_name. The SSL_get_cipher_name macro calls
// SSL_CIPHER_get_name(SSL_get_current_cipher(s)), so this will exercise the
// branch where no cipher name is recorded.
extern "C" const char* SSL_CIPHER_get_name(const SSL_CIPHER* /*c*/) { return nullptr; }

// Provide a test-local SSL_get_version override that returns a non-null string
// except when g_tls_handshake_test_mode == 10 (reserved to force nullptr).

// Global mode to control test interposed behaviors:
// 0 = default (no special behavior),
// 1 = SSL_get_peer_certificate returns X509 and X509_get_subject_name returns nullptr
// 2 = SSL_get_peer_certificate returns X509 and X509_NAME_print_ex fails (<0)
// 3 = SSL_get_peer_certificate returns X509, X509_NAME_print_ex succeeds, but BIO_get_mem_ptr leaves bptr=null

extern "C" const char* SSL_get_version(const SSL* /*s*/) {
  if (g_tls_handshake_test_mode == 10) {
    return nullptr;
  }
  if (g_tls_handshake_test_mode == 11) {
    return "";
  }
  return "TLS-TEST";
}

extern "C" X509* SSL_get_peer_certificate(const SSL* /*s*/) {
  if (g_tls_handshake_test_mode >= 1) {
    ensure_test_x509_created();
    return g_test_x509;
  }
  return nullptr;
}

extern "C" X509_NAME* X509_get_subject_name(const X509* /*x") */) {
  if (g_tls_handshake_test_mode == 1) {
    return nullptr;
  }
  ensure_test_x509_created();
  return g_test_x509_name;
}

extern "C" int X509_NAME_print_ex(BIO* bp, const X509_NAME* name, int indent, unsigned long flags) {
  if (g_tls_handshake_test_mode == 2) {
    return -1;  // simulate failure
  }
  // Simple emulation: write a short subject string into BIO and return length
  (void)name;
  (void)indent;
  (void)flags;
  const char* str = "CN=test";
  return ::BIO_write(bp, str, static_cast<int>(strlen(str)));
}

// Interpose BIO_ctrl to simulate BIO_get_mem_ptr failing to set BUF_MEM pointer
// when g_tls_handshake_test_mode == 3. We use dlsym(RTLD_NEXT, ...) to call
// the real implementation for other commands.
extern "C" long BIO_ctrl(BIO* bp, int cmd, long larg, void* parg) {
  using bio_ctrl_fn = long (*)(BIO*, int, long, void*);
  static bio_ctrl_fn real_bio_ctrl = nullptr;
  if (real_bio_ctrl == nullptr) {
    real_bio_ctrl = reinterpret_cast<bio_ctrl_fn>(dlsym(RTLD_NEXT, "BIO_ctrl"));
    if (real_bio_ctrl == nullptr) {
      // If we can't find the real BIO_ctrl, fail-safe: return 0
      return 0;
    }
  }
  if (cmd == BIO_C_GET_BUF_MEM_PTR && g_tls_handshake_test_mode == 3) {
    if (parg != nullptr) {
      *(reinterpret_cast<BUF_MEM**>(parg)) = nullptr;
    }
    return 0;
  }
  return real_bio_ctrl(bp, cmd, larg, parg);
}

namespace aeronet {

TEST(TlsHandshakeTest, MaybeEnableKtlsSendUnsupported) {
  TlsMetricsInternal metrics;
  const int fd = 42;  // Dummy file descriptor
  const auto result =
      MaybeEnableKtlsSend(KtlsEnableResult::Unsupported, fd, TLSConfig::KtlsMode::Opportunistic, metrics);

  EXPECT_EQ(result, KtlsApplication::Disabled);
  EXPECT_EQ(metrics.ktlsSendForcedShutdowns, 0);
}

TEST(TlsHandshakeTest, MaybeEnableKtlsSendEnabled) {
  TlsMetricsInternal metrics;
  const int fd = 7;
  const auto result = MaybeEnableKtlsSend(KtlsEnableResult::Enabled, fd, TLSConfig::KtlsMode::Opportunistic, metrics);

  EXPECT_EQ(result, KtlsApplication::Enabled);
  EXPECT_EQ(metrics.ktlsSendEnabledConnections, 1);
  EXPECT_EQ(metrics.ktlsSendEnableFallbacks, 0);
  EXPECT_EQ(metrics.ktlsSendForcedShutdowns, 0);
}

TEST(TlsHandshakeTest, MaybeEnableKtlsSendUnsupportedWarnMode) {
  TlsMetricsInternal metrics;
  const int fd = 8;
  const auto result = MaybeEnableKtlsSend(KtlsEnableResult::Unsupported, fd, TLSConfig::KtlsMode::Enabled, metrics);

  EXPECT_EQ(result, KtlsApplication::Disabled);
  EXPECT_EQ(metrics.ktlsSendEnableFallbacks, 1);
  EXPECT_EQ(metrics.ktlsSendForcedShutdowns, 0);
}

TEST(TlsHandshakeTest, MaybeEnableKtlsSendUnsupportedForced) {
  TlsMetricsInternal metrics;
  const int fd = 9;
  const auto result = MaybeEnableKtlsSend(KtlsEnableResult::Unsupported, fd, TLSConfig::KtlsMode::Required, metrics);

  EXPECT_EQ(result, KtlsApplication::CloseConnection);
  EXPECT_EQ(metrics.ktlsSendEnableFallbacks, 1);
  EXPECT_EQ(metrics.ktlsSendForcedShutdowns, 1);
}

TEST(TlsHandshakeTest, MaybeEnableKtlsSendDisabledOpportunistic) {
  TlsMetricsInternal metrics;
  const int fd = 10;
  const auto result = MaybeEnableKtlsSend(KtlsEnableResult::Disabled, fd, TLSConfig::KtlsMode::Opportunistic, metrics);

  EXPECT_EQ(result, KtlsApplication::Disabled);
  EXPECT_EQ(metrics.ktlsSendEnableFallbacks, 1);
  EXPECT_EQ(metrics.ktlsSendForcedShutdowns, 0);
}

TEST(TlsHandshakeTest, MaybeEnableKtlsSendUnknownRequired) {
  TlsMetricsInternal metrics;
  const int fd = 11;
  const auto result = MaybeEnableKtlsSend(KtlsEnableResult::Unknown, fd, TLSConfig::KtlsMode::Required, metrics);

  EXPECT_EQ(result, KtlsApplication::CloseConnection);
  EXPECT_EQ(metrics.ktlsSendEnableFallbacks, 1);
  EXPECT_EQ(metrics.ktlsSendForcedShutdowns, 1);
}

TEST(TlsHandshakeTest, FinalizeTlsHandshake_NoCipherName) {
  // Create an SSL object but do not perform a handshake; OpenSSL should return nullptr for cipher name
  TlsMetricsInternal metrics;
  SslCtxPtr ctx(::SSL_CTX_new(TLS_method()), ::SSL_CTX_free);
  ASSERT_TRUE(ctx);
  SslPtr ssl(::SSL_new(ctx.get()), ::SSL_free);
  ASSERT_TRUE(ssl);

  // Do not associate a socket fd; use dummy fd -1
  const int dummyFd = -1;

  // Call FinalizeTlsHandshake which internally calls CollectAndLogTlsHandshake and should handle nullptr cipher
  TLSInfo info = FinalizeTlsHandshake(ssl.get(), dummyFd, false /*logHandshake*/, false /*tlsHandshakeEventEmitted*/,
                                      TlsHandshakeCallback(), std::chrono::steady_clock::time_point{}, metrics);

  // negotiatedCipher should be empty when SSL_get_cipher_name returns nullptr
  EXPECT_TRUE(info.negotiatedCipher().empty());
  // Metrics should have one successful handshake counted
  EXPECT_EQ(metrics.handshakesSucceeded, 1);
}

TEST(TlsHandshakeTest, FinalizeTlsHandshake_PeerSubject_Absent) {
  // mode 1: peer certificate present but subject name is null
  g_tls_handshake_test_mode = 1;
  TlsMetricsInternal metrics;
  SslCtxPtr ctx(::SSL_CTX_new(TLS_method()), ::SSL_CTX_free);
  ASSERT_TRUE(ctx);
  SslPtr ssl(::SSL_new(ctx.get()), ::SSL_free);
  ASSERT_TRUE(ssl);

  TLSInfo info = FinalizeTlsHandshake(ssl.get(), -1, false, false, TlsHandshakeCallback(),
                                      std::chrono::steady_clock::time_point{}, metrics);
  EXPECT_TRUE(info.peerSubject().empty());
  EXPECT_EQ(metrics.handshakesSucceeded, 1);
  g_tls_handshake_test_mode = 0;
}

TEST(TlsHandshakeTest, FinalizeTlsHandshake_PeerSubject_PrintFail) {
  // mode 2: printing subject fails
  g_tls_handshake_test_mode = 2;
  TlsMetricsInternal metrics;
  SslCtxPtr ctx(::SSL_CTX_new(TLS_method()), ::SSL_CTX_free);
  ASSERT_TRUE(ctx);
  SslPtr ssl(::SSL_new(ctx.get()), ::SSL_free);
  ASSERT_TRUE(ssl);

  TLSInfo info = FinalizeTlsHandshake(ssl.get(), -1, false, false, TlsHandshakeCallback(),
                                      std::chrono::steady_clock::time_point{}, metrics);
  EXPECT_TRUE(info.peerSubject().empty());
  EXPECT_EQ(metrics.handshakesSucceeded, 1);
  g_tls_handshake_test_mode = 0;
}

TEST(TlsHandshakeTest, FinalizeTlsHandshake_PeerSubject_BioPtrNull) {
  // mode 3: printing succeeds but BIO_get_mem_ptr leaves bptr == nullptr
  g_tls_handshake_test_mode = 3;
  TlsMetricsInternal metrics;
  SslCtxPtr ctx(::SSL_CTX_new(TLS_method()), ::SSL_CTX_free);
  ASSERT_TRUE(ctx);
  SslPtr ssl(::SSL_new(ctx.get()), ::SSL_free);
  ASSERT_TRUE(ssl);

  TLSInfo info = FinalizeTlsHandshake(ssl.get(), -1, false, false, TlsHandshakeCallback(),
                                      std::chrono::steady_clock::time_point{}, metrics);
  EXPECT_TRUE(info.peerSubject().empty());
  EXPECT_EQ(metrics.handshakesSucceeded, 1);
  g_tls_handshake_test_mode = 0;
}

TEST(TlsHandshakeTest, FinalizeTlsHandshake_NoVersionStringEmpty) {
  // mode 11: SSL_get_version returns empty string
  g_tls_handshake_test_mode = 11;
  TlsMetricsInternal metrics;
  SslCtxPtr ctx(::SSL_CTX_new(TLS_method()), ::SSL_CTX_free);
  ASSERT_TRUE(ctx);
  SslPtr ssl(::SSL_new(ctx.get()), ::SSL_free);
  ASSERT_TRUE(ssl);

  TLSInfo info = FinalizeTlsHandshake(ssl.get(), -1, false, false, TlsHandshakeCallback(),
                                      std::chrono::steady_clock::time_point{}, metrics);
  EXPECT_TRUE(info.negotiatedVersion().empty());
  // versionCounts should not have an entry for an empty version
  EXPECT_EQ(metrics.versionCounts.size(), 0U);
  EXPECT_EQ(metrics.handshakesSucceeded, 1);
  g_tls_handshake_test_mode = 0;
}

TEST(TlsHandshakeTest, FinalizeTlsHandshake_NoDurationRecorded) {
  // When handshakeStart is epoch, duration should remain 0 and metrics not updated for duration
  TlsMetricsInternal metrics;
  SslCtxPtr ctx(::SSL_CTX_new(TLS_method()), ::SSL_CTX_free);
  ASSERT_TRUE(ctx);
  SslPtr ssl(::SSL_new(ctx.get()), ::SSL_free);
  ASSERT_TRUE(ssl);

  // Pass a default-constructed time_point (epoch)
  TLSInfo info = FinalizeTlsHandshake(ssl.get(), -1, false, true, TlsHandshakeCallback(),
                                      std::chrono::steady_clock::time_point{}, metrics);
  EXPECT_EQ(metrics.handshakeDurationCount, 0U);
  EXPECT_EQ(metrics.handshakeDurationTotalNs, 0U);
  EXPECT_EQ(metrics.handshakeDurationMaxNs, 0U);
  EXPECT_EQ(metrics.handshakesSucceeded, 1);
}

}  // namespace aeronet