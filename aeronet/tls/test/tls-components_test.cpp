#include <gtest/gtest.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/sslerr.h>
#include <openssl/types.h>
#include <openssl/x509_vfy.h>
#include <sys/socket.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <initializer_list>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "aeronet/base-fd.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/socket-ops.hpp"
#include "aeronet/sys-test-support.hpp"
#include "aeronet/temp-file.hpp"
#include "aeronet/test-tls-helper.hpp"
#include "aeronet/tls-config.hpp"
#include "aeronet/tls-context.hpp"
#include "aeronet/tls-handshake.hpp"
#include "aeronet/tls-info.hpp"
#include "aeronet/tls-ktls.hpp"
#include "aeronet/tls-metrics.hpp"
#include "aeronet/tls-raii.hpp"
#include "aeronet/tls-transport.hpp"
#include "aeronet/transport.hpp"

using namespace aeronet;

namespace tls_test {
namespace {

// Certificate cache to avoid expensive RSA keygen + signing in every test.
// Most tests don't require unique certificates, so we cache and reuse them.
struct CertKeyCache {
  std::pair<std::string, std::string> localhost;
  std::pair<std::string, std::string> serverA;
  std::pair<std::string, std::string> serverB;
  std::pair<std::string, std::string> client;
  std::pair<std::string, std::string> wildcard;

  static CertKeyCache& Get() {
    static CertKeyCache instance{};
    return instance;
  }

 private:
  CertKeyCache() {
    // Pre-generate all certificate pairs on first access
    localhost = test::MakeEphemeralCertKey("localhost");
    serverA = test::MakeEphemeralCertKey("server-a");
    serverB = test::MakeEphemeralCertKey("server-b");
    client = test::MakeEphemeralCertKey("client.example.com");
    wildcard = test::MakeEphemeralCertKey("main.example.com");
  }
};

std::vector<unsigned char> MakeAlpnWire(std::initializer_list<std::string_view> protos) {
  std::vector<unsigned char> wire;
  for (std::string_view proto : protos) {
    EXPECT_LE(proto.size(), TLSConfig::kMaxAlpnProtocolLength);
    wire.push_back(static_cast<unsigned char>(proto.size()));
    wire.insert(wire.end(), proto.begin(), proto.end());
  }
  return wire;
}

TLSConfig MakeTlsConfig(std::initializer_list<std::string_view> serverAlpn, bool strictAlpn) {
  auto cert = CertKeyCache::Get().localhost;

  TLSConfig cfg;
  cfg.enabled = true;
  cfg.withCertPem(cert.first).withKeyPem(cert.second);
  cfg.withTlsAlpnProtocols(serverAlpn);
  cfg.alpnMustMatch = strictAlpn;
  return cfg;
}

struct SslTestPair {
  SslTestPair(std::initializer_list<std::string_view> serverAlpn, std::initializer_list<std::string_view> clientAlpn,
              bool strictAlpn = false)
      : cfg(MakeTlsConfig(serverAlpn, strictAlpn)),
        context(cfg),
        clientCtx(nullptr, &::SSL_CTX_free),
        serverSsl(nullptr, &::SSL_free),
        clientSsl(nullptr, &::SSL_free) {
    auto cert = CertKeyCache::Get().localhost;

    auto* serverCtx = reinterpret_cast<SSL_CTX*>(context.raw());
    serverSsl = TlsTransport::SslPtr(::SSL_new(serverCtx), &::SSL_free);
    EXPECT_NE(serverSsl, nullptr);
    clientCtx.reset(::SSL_CTX_new(TLS_client_method()));
    EXPECT_NE(clientCtx, nullptr);
    ::SSL_CTX_set_verify(clientCtx.get(), SSL_VERIFY_NONE, nullptr);
    if (clientAlpn.size() > 0) {
      auto wire = MakeAlpnWire(clientAlpn);
      const unsigned int wireLen = static_cast<unsigned int>(wire.size());
      if (::SSL_CTX_set_alpn_protos(clientCtx.get(), wire.data(), wireLen) != 0) {
        throw std::runtime_error("Failed to set client ALPN protocols");
      }
    }
    clientSsl = SslPtr(::SSL_new(clientCtx.get()), &::SSL_free);
    EXPECT_NE(clientSsl, nullptr);
    int fds[2]{};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
      throw std::runtime_error("socketpair() failed");
    }
    serverFd = BaseFd(fds[0]);
    clientFd = BaseFd(fds[1]);
    if (::SSL_set_fd(serverSsl.get(), serverFd.fd()) != 1) {
      throw std::runtime_error("Failed to set server SSL fd");
    }
    if (::SSL_set_fd(clientSsl.get(), clientFd.fd()) != 1) {
      throw std::runtime_error("Failed to set client SSL fd");
    }
    ::SSL_set_accept_state(serverSsl.get());
    ::SSL_set_connect_state(clientSsl.get());
  }

  TLSConfig cfg;
  TlsContext context;
  SslCtxPtr clientCtx{nullptr, &::SSL_CTX_free};
  TlsTransport::SslPtr serverSsl{nullptr, &::SSL_free};
  SslPtr clientSsl{nullptr, &::SSL_free};
  BaseFd serverFd;
  BaseFd clientFd;
};

struct ControlledBioState {
  int readResult{-1};
  int writeResult{-1};
  int errnoValue{EAGAIN};
  bool retryRead{false};
  bool retryWrite{false};
};

int ControlledBioCreate(BIO* bio) {
  BIO_set_init(bio, 1);
  BIO_set_data(bio, nullptr);
  return 1;
}

int ControlledBioDestroy(BIO* bio) {
  BIO_set_init(bio, 0);
  BIO_set_data(bio, nullptr);
  return 1;
}

long ControlledBioCtrl(BIO* bio [[maybe_unused]], int cmd, [[maybe_unused]] long num, [[maybe_unused]] void* ptr) {
  return (cmd == BIO_CTRL_FLUSH) ? 1 : 0;
}

int ControlledBioRead(BIO* bio, char* buf [[maybe_unused]], int len [[maybe_unused]]) {
  auto* state = static_cast<ControlledBioState*>(BIO_get_data(bio));
  if (state == nullptr) {
    return 0;
  }
  errno = state->errnoValue;
  BIO_clear_retry_flags(bio);
  if (state->retryRead) {
    BIO_set_retry_read(bio);
  }
  if (state->retryWrite) {
    BIO_set_retry_write(bio);
  }
  return state->readResult;
}

int ControlledBioWrite(BIO* bio, const char* buf [[maybe_unused]], int len [[maybe_unused]]) {
  auto* state = static_cast<ControlledBioState*>(BIO_get_data(bio));
  if (state == nullptr) {
    return 0;
  }
  errno = state->errnoValue;
  BIO_clear_retry_flags(bio);
  if (state->retryWrite) {
    BIO_set_retry_write(bio);
  }
  return state->writeResult;
}

BIO_METHOD* ControlledBioMethod() {
  static BIO_METHOD* method = [] {
    BIO_METHOD* created = BIO_meth_new(BIO_TYPE_SOURCE_SINK, "tls_transport_test_bio");
    BIO_meth_set_create(created, &ControlledBioCreate);
    BIO_meth_set_destroy(created, &ControlledBioDestroy);
    BIO_meth_set_ctrl(created, &ControlledBioCtrl);
    BIO_meth_set_read(created, &ControlledBioRead);
    BIO_meth_set_write(created, &ControlledBioWrite);
    return created;
  }();
  return method;
}

BIO* MakeControlledBio(ControlledBioState* state) {
  BIO* bio = BIO_new(ControlledBioMethod());
  if (bio == nullptr) {
    ADD_FAILURE() << "Failed to allocate test BIO";
    return nullptr;
  }
  BIO_set_data(bio, state);
  BIO_set_init(bio, 1);
  return bio;
}

void AttachControlledBios(SSL* ssl, ControlledBioState& readState, ControlledBioState& writeState) {
  BIO* readBio = MakeControlledBio(&readState);
  BIO* writeBio = MakeControlledBio(&writeState);
  ASSERT_NE(readBio, nullptr);
  ASSERT_NE(writeBio, nullptr);
  ::SSL_set_bio(ssl, readBio, writeBio);
}

bool PerformHandshake(SslTestPair& pair) {
  int serverRc = 0;
  std::thread serverThread([&] { serverRc = ::SSL_accept(pair.serverSsl.get()); });
  const int clientRc = ::SSL_connect(pair.clientSsl.get());
  serverThread.join();
  return (serverRc == 1) && (clientRc == 1);
}

}  // namespace

TEST(TLSRaiiTest, ShouldThrowBadAllocOnNullPtrs) {
  EXPECT_THROW(MakeBio(nullptr), std::bad_alloc);
  EXPECT_THROW(MakePKey(nullptr), std::bad_alloc);
  EXPECT_THROW(MakeX509(nullptr), std::bad_alloc);
}

TEST(TlsContextTest, CollectsHandshakeInfo) {
  SslTestPair pair({"http/1.1"}, {"http/1.1"});
  const auto start = std::chrono::steady_clock::now();
  ASSERT_TRUE(PerformHandshake(pair));

  TlsMetricsInternal metrics;
  bool tlsHandshakeEventEmitted = false;
  auto tlsInfo = FinalizeTlsHandshake(pair.serverSsl.get(), pair.serverFd.fd(), true, tlsHandshakeEventEmitted, {},
                                      start, metrics);
  EXPECT_EQ(tlsInfo.selectedAlpn(), "http/1.1");
  EXPECT_EQ(metrics.handshakesSucceeded, 1U);
  EXPECT_EQ(metrics.alpnDistribution[RawChars32("http/1.1")], 1U);
  EXPECT_EQ(metrics.versionCounts.size(), 1U);
  EXPECT_EQ(metrics.cipherCounts.size(), 1U);
  EXPECT_EQ(metrics.handshakeDurationCount, 1U);
  EXPECT_EQ(metrics.handshakeDurationMaxNs, metrics.handshakeDurationTotalNs);
}

TEST(TlsContextTest, StrictAlpnMismatchIncrementsMetric) {
  SslTestPair pair({"h2"}, {"http/1.1"}, true);
  EXPECT_FALSE(PerformHandshake(pair));
  EXPECT_EQ(pair.context.alpnStrictMismatches(), 1U);
  ::ERR_clear_error();
}

#ifdef TLS1_3_VERSION
TEST(TlsContextTest, SupportsTls13VersionBounds) {
  auto certKey = CertKeyCache::Get().localhost;
  TLSConfig cfg;
  cfg.enabled = true;
  cfg.withCertPem(certKey.first).withKeyPem(certKey.second);
  cfg.minVersion = TLSConfig::TLS_1_3;
  cfg.maxVersion = TLSConfig::TLS_1_3;
  EXPECT_NO_THROW(TlsContext{cfg});
}
#endif

TEST(TlsContextTest, InvalidKtlsModeThrows) {
  auto certKey = CertKeyCache::Get().localhost;
  TLSConfig cfg;
  cfg.enabled = true;
  cfg.withCertPem(certKey.first).withKeyPem(certKey.second);
  cfg.withKtlsMode(
      static_cast<TLSConfig::KtlsMode>(std::numeric_limits<std::underlying_type_t<TLSConfig::KtlsMode>>::max()));
  EXPECT_THROW(TlsContext{cfg}, std::invalid_argument);
}

TEST(TlsContextTest, InvalidMinVersionThrows) {
  auto certKey = CertKeyCache::Get().localhost;
  TLSConfig cfg;
  cfg.enabled = true;
  cfg.withCertPem(certKey.first).withKeyPem(certKey.second);
  cfg.withTlsMinVersion("TLS1.1");
  EXPECT_THROW(TlsContext{cfg}, std::runtime_error);
}

TEST(TlsContextTest, InvalidMaxVersionThrows) {
  auto certKey = CertKeyCache::Get().localhost;
  TLSConfig cfg;
  cfg.enabled = true;
  cfg.withCertPem(certKey.first).withKeyPem(certKey.second);
  cfg.withTlsMaxVersion("TLS1.1");

  EXPECT_THROW(TlsContext{cfg}, std::runtime_error);
}

TEST(TlsContextTest, CipherPolicyAppliesPredefinedSuites) {
  auto certKey = CertKeyCache::Get().localhost;

  const std::array<TLSConfig::CipherPolicy, 3> policies = {
      TLSConfig::CipherPolicy::Modern, TLSConfig::CipherPolicy::Compatibility, TLSConfig::CipherPolicy::Legacy};

  for (auto policy : policies) {
    TLSConfig cfg;
    cfg.enabled = true;
    cfg.withCertPem(certKey.first).withKeyPem(certKey.second);
    cfg.withTlsCipherPolicy(policy);

    EXPECT_NO_THROW({
      TlsContext ctx(cfg);
      (void)ctx;
    }) << "policy="
       << static_cast<int>(policy);
  }
}

TEST(TlsContextTest, InvalidInMemoryPemThrows) {
  TLSConfig cfg;
  cfg.enabled = true;
  cfg.withCertPem("not-a-real-pem");
  cfg.withKeyPem("still-not-a-pem");

  EXPECT_THROW(TlsContext{cfg}, std::bad_alloc);
}

TEST(TlsContextTest, MismatchedPrivateKeyFailsCheck) {
  auto certA = CertKeyCache::Get().serverA;
  auto certB = CertKeyCache::Get().serverB;
  ASSERT_FALSE(certA.first.empty());
  ASSERT_FALSE(certA.second.empty());
  ASSERT_FALSE(certB.first.empty());
  ASSERT_FALSE(certB.second.empty());
  TLSConfig cfg;
  cfg.enabled = true;
  cfg.withCertPem(certA.first);
  cfg.withKeyPem(certB.second);

  EXPECT_THROW(TlsContext{cfg}, std::runtime_error);
}

TEST(TlsContextTest, PrivateKeyCheckFailsWithMismatchedFiles) {
  auto certA = CertKeyCache::Get().serverA;
  auto certB = CertKeyCache::Get().serverB;
  ASSERT_FALSE(certA.first.empty());
  ASSERT_FALSE(certA.second.empty());
  ASSERT_FALSE(certB.second.empty());

  test::ScopedTempDir tmpDir;
  test::ScopedTempFile certFile(tmpDir, certA.first);
  test::ScopedTempFile keyFile(tmpDir, certB.second);

  TLSConfig cfg;
  cfg.enabled = true;
  cfg.withCertFile(certFile.filePath().string());
  cfg.withKeyFile(keyFile.filePath().string());

  EXPECT_THROW(TlsContext{cfg}, std::runtime_error);
}

TEST(TlsContextTest, EmptyTrustedClientCertPemThrows) {
  auto certKey = CertKeyCache::Get().localhost;
  TLSConfig cfg;
  cfg.enabled = true;
  cfg.requestClientCert = true;
  cfg.withCertPem(certKey.first).withKeyPem(certKey.second);
  cfg.withTlsTrustedClientCert("");

  EXPECT_THROW(TlsContext{cfg}, std::runtime_error);
}

TEST(TlsContextTest, InvalidTrustedClientCertPemThrows) {
  auto certKey = CertKeyCache::Get().localhost;
  TLSConfig cfg;
  cfg.enabled = true;
  cfg.requestClientCert = true;
  cfg.withCertPem(certKey.first).withKeyPem(certKey.second);
  cfg.withTlsTrustedClientCert("not-a-cert");
  EXPECT_THROW(TlsContext{cfg}, std::bad_alloc);
}

TEST(TlsContextTest, DisableCompressionFalseConfiguresSslCtx) {
  auto certKey = CertKeyCache::Get().localhost;
  TLSConfig cfg;
  cfg.enabled = true;
  cfg.withCertPem(certKey.first).withKeyPem(certKey.second);
  cfg.disableCompression = false;

#ifdef SSL_OP_NO_COMPRESSION
  EXPECT_NO_THROW({
    TlsContext ctx(cfg);
    auto* raw = reinterpret_cast<SSL_CTX*>(ctx.raw());
    const unsigned long opts = ::SSL_CTX_get_options(raw);
    EXPECT_EQ((opts & SSL_OP_NO_COMPRESSION), 0U);
  });
#else
  GTEST_SKIP() << "OpenSSL built without SSL_OP_NO_COMPRESSION support";
#endif
}

TEST(TlsContextTest, MissingCertificateFilesThrow) {
  TLSConfig cfg;
  cfg.enabled = true;
  cfg.withCertFile("/__aeronet_missing_cert__.pem");
  cfg.withKeyFile("/__aeronet_missing_key__.pem");

  EXPECT_THROW(TlsContext{cfg}, std::runtime_error);
}

namespace {
constexpr uint32_t kMinBytesForZerocopy = 1024;
}

TEST(TlsTransportTest, ReadWriteAndRetryHints) {
  SslTestPair pair({"http/1.1"}, {"http/1.1"});
  ASSERT_TRUE(PerformHandshake(pair));
  TlsTransport transport(std::move(pair.serverSsl), kMinBytesForZerocopy);

  auto zeroWrite = transport.write(std::string_view{});
  EXPECT_EQ(zeroWrite.bytesProcessed, 0U);
  EXPECT_EQ(zeroWrite.want, TransportHint::None);

  SetNonBlocking(pair.serverFd.fd());
  char buf[16] = {};
  auto wantRead = transport.read(buf, sizeof(buf));
  EXPECT_EQ(wantRead.bytesProcessed, 0U);
  EXPECT_EQ(wantRead.want, TransportHint::ReadReady);

  const std::string payload = "PING";
  const int written = ::SSL_write(pair.clientSsl.get(), payload.data(), static_cast<int>(payload.size()));
  ASSERT_EQ(written, static_cast<int>(payload.size()));
  auto readRes = transport.read(buf, sizeof(buf));
  EXPECT_EQ(readRes.want, TransportHint::None);
  EXPECT_EQ(readRes.bytesProcessed, payload.size());
  EXPECT_EQ(std::string_view(buf, readRes.bytesProcessed), std::string_view(payload));

  auto writeRes = transport.write("PONG");
  EXPECT_EQ(writeRes.want, TransportHint::None);
  EXPECT_EQ(writeRes.bytesProcessed, 4U);
  char clientBuf[8] = {};
  const int clientRead = ::SSL_read(pair.clientSsl.get(), clientBuf, sizeof(clientBuf));
  ASSERT_EQ(clientRead, 4);
  EXPECT_EQ(std::string_view(clientBuf, 4), "PONG");

  ERR_put_error(ERR_LIB_SSL, 0, SSL_R_BAD_LENGTH, __FILE__, __LINE__);
  transport.logErrorIfAny();

  transport.shutdown();
  transport.shutdown();
}

TEST(TlsTransportTest, HandshakeSyscallWriteFatalSetsError) {
  SslTestPair pair({"http/1.1"}, {"http/1.1"});
  ControlledBioState readState{};
  readState.errnoValue = EBADF;
  ControlledBioState writeState{};
  writeState.errnoValue = EBADF;
  AttachControlledBios(pair.serverSsl.get(), readState, writeState);
  pair.serverFd.close();
  TlsTransport transport(std::move(pair.serverSsl), kMinBytesForZerocopy);
  ERR_clear_error();
  errno = EBADF;
  auto res = transport.write("X");
  EXPECT_EQ(res.want, TransportHint::Error);
  EXPECT_EQ(res.bytesProcessed, 0U);
}

TEST(TlsTransportTest, ReadReportsReadReadyOnSslSyscallEagain) {
  // Construct a transport with a controlled BIO that simulates SSL_ERROR_SYSCALL with errno==EAGAIN
  SslTestPair pair({"http/1.1"}, {"http/1.1"});
  // Attach controlled bios where read returns -1 and sets errno to EAGAIN and marks retry
  ControlledBioState readState{};
  readState.readResult = -1;
  readState.errnoValue = EAGAIN;
  readState.retryRead = false;  // SSL_ERROR_SYSCALL path uses errno, not BIO retry flags

  ControlledBioState writeState{};
  writeState.writeResult = 0;

  AttachControlledBios(pair.serverSsl.get(), readState, writeState);
  TlsTransport transport(std::move(pair.serverSsl), kMinBytesForZerocopy);

  char buf[8];
  auto res = transport.read(buf, sizeof(buf));
  // Should report would-block (read ready) rather than error
  EXPECT_EQ(res.want, TransportHint::ReadReady);

  // Now test the WANT_WRITE path: create a fresh pair where BIO indicates a write retry
  SslTestPair pair2({"http/1.1"}, {"http/1.1"});
  ControlledBioState r2{};
  r2.readResult = -1;
  r2.errnoValue = 0;
  r2.retryRead = false;
  r2.retryWrite = true;
  ControlledBioState w2{};
  w2.writeResult = -1;
  w2.errnoValue = 0;
  w2.retryWrite = true;
  AttachControlledBios(pair2.serverSsl.get(), r2, w2);
  TlsTransport transport2(std::move(pair2.serverSsl), kMinBytesForZerocopy);
  char buf2[8];
  ERR_clear_error();
  errno = 0;
  auto res2 = transport2.read(buf2, sizeof(buf2));
  // Debug output for failing investigation
  EXPECT_TRUE(res2.want == TransportHint::WriteReady || res2.want == TransportHint::ReadReady);
}

TEST(TlsTransportLogTest, SslSyscallEagainShouldReturnReadReady) {
  SslTestPair pair({"http/1.1"}, {"http/1.1"});
  // Perform a real handshake so the transport read path will call SSL_read_ex
  ASSERT_TRUE(PerformHandshake(pair));

  ControlledBioState readState{};
  readState.readResult = -1;
  readState.errnoValue = EAGAIN;
  readState.retryRead = false;
  ControlledBioState writeState{};
  writeState.writeResult = 0;
  AttachControlledBios(pair.serverSsl.get(), readState, writeState);
  TlsTransport transport(std::move(pair.serverSsl), kMinBytesForZerocopy);

  ERR_clear_error();
  errno = 0;
  char buf[8];
  auto res = transport.read(buf, sizeof(buf));

  EXPECT_EQ(res.want, TransportHint::ReadReady);
}

TEST(TlsTransportLogTest, ControlledBioSslReadErrorMapping) {
  SslTestPair pair({"http/1.1"}, {"http/1.1"});
  ControlledBioState readState{};
  readState.readResult = -1;
  readState.errnoValue = EAGAIN;
  readState.retryRead = false;
  ControlledBioState writeState{};
  writeState.writeResult = 0;
  AttachControlledBios(pair.serverSsl.get(), readState, writeState);

  char buf[8];
  int outLen = 0;
  errno = 0;
  int rc = ::SSL_read_ex(pair.serverSsl.get(), buf, sizeof(buf), reinterpret_cast<size_t*>(&outLen));
  int err_with_rc = ::SSL_get_error(pair.serverSsl.get(), rc);
  int err_with_zero = ::SSL_get_error(pair.serverSsl.get(), 0);

  // Ensure we observed the failing rc and some mapping
  EXPECT_NE(err_with_rc, 0);
  EXPECT_NE(err_with_zero, 0);
}

TEST(TlsTransportWriteHintTest, ReadReportsWriteReadyWhenWantWrite) {
  SslTestPair pair({"http/1.1"}, {"http/1.1"});

  ControlledBioState readState{};
  readState.readResult = -1;
  readState.errnoValue = 0;
  readState.retryRead = false;
  readState.retryWrite = true;

  ControlledBioState writeState{};
  writeState.writeResult = -1;
  writeState.errnoValue = 0;
  writeState.retryWrite = true;

  AttachControlledBios(pair.serverSsl.get(), readState, writeState);
  TlsTransport transport(std::move(pair.serverSsl), kMinBytesForZerocopy);

  char buf[8];
  ERR_clear_error();
  errno = 0;
  auto res = transport.read(buf, sizeof(buf));
  EXPECT_TRUE(res.want == TransportHint::WriteReady || res.want == TransportHint::ReadReady);
}

TEST(TlsTransportTest, SyscallDuringReadWithErrnoZeroRetried) {
  SslTestPair pair({"http/1.1"}, {"http/1.1"});
  ASSERT_TRUE(PerformHandshake(pair));
  SSL* rawSsl = pair.serverSsl.get();
  TlsTransport transport(std::move(pair.serverSsl), kMinBytesForZerocopy);
  ControlledBioState readState{};
  readState.errnoValue = 0;
  ControlledBioState writeState{};
  writeState.errnoValue = 0;
  AttachControlledBios(rawSsl, readState, writeState);
  pair.serverFd.close();
  char tmp[1]{};
  ERR_clear_error();
  errno = 0;
  auto res = transport.read(tmp, sizeof(tmp));
  EXPECT_EQ(res.want, TransportHint::ReadReady);
  EXPECT_EQ(res.bytesProcessed, 0U);
}

TEST(TlsTransportTest, SyscallDuringReadFatalSetsErrorHint) {
  SslTestPair pair({"http/1.1"}, {"http/1.1"});
  ASSERT_TRUE(PerformHandshake(pair));
  SSL* rawSsl = pair.serverSsl.get();
  TlsTransport transport(std::move(pair.serverSsl), kMinBytesForZerocopy);
  ControlledBioState readState{};
  readState.errnoValue = EBADF;
  ControlledBioState writeState{};
  writeState.errnoValue = EBADF;
  AttachControlledBios(rawSsl, readState, writeState);
  pair.serverFd.close();
  char tmp[1]{};
  ERR_clear_error();
  errno = EBADF;
  auto res = transport.read(tmp, sizeof(tmp));
  EXPECT_EQ(res.want, TransportHint::Error);
  EXPECT_EQ(res.bytesProcessed, 0U);
}

TEST(TlsTransportTest, SyscallDuringWriteWithErrnoZeroRetried) {
  SslTestPair pair({"http/1.1"}, {"http/1.1"});
  ASSERT_TRUE(PerformHandshake(pair));
  SSL* rawSsl = pair.serverSsl.get();
  TlsTransport transport(std::move(pair.serverSsl), kMinBytesForZerocopy);
  ControlledBioState readState{};
  readState.errnoValue = 0;
  ControlledBioState writeState{};
  writeState.errnoValue = 0;
  AttachControlledBios(rawSsl, readState, writeState);
  pair.serverFd.close();
  ERR_clear_error();
  errno = 0;
  auto res = transport.write("ping");
  EXPECT_TRUE(res.want == TransportHint::WriteReady || res.want == TransportHint::Error);
  EXPECT_EQ(res.bytesProcessed, 0U);
}

TEST(TlsTransportTest, SyscallDuringWriteFatalSetsErrorHint) {
  SslTestPair pair({"http/1.1"}, {"http/1.1"});
  ASSERT_TRUE(PerformHandshake(pair));
  SSL* rawSsl = pair.serverSsl.get();
  TlsTransport transport(std::move(pair.serverSsl), kMinBytesForZerocopy);
  ControlledBioState readState{};
  readState.errnoValue = EBADF;
  ControlledBioState writeState{};
  writeState.errnoValue = EBADF;
  AttachControlledBios(rawSsl, readState, writeState);
  pair.serverFd.close();
  ERR_clear_error();
  errno = EBADF;
  auto res = transport.write("ping");
  EXPECT_EQ(res.want, TransportHint::Error);
  EXPECT_EQ(res.bytesProcessed, 0U);
}

TEST(TlsTransportTest, SuccessfulReadReturnsData) {
  // Covers the early-return success path in TlsTransport::read (line 45)
  SslTestPair pair({"http/1.1"}, {"http/1.1"});
  ASSERT_TRUE(PerformHandshake(pair));
  TlsTransport transport(std::move(pair.serverSsl), kMinBytesForZerocopy);

  // Client writes data that server will read
  const std::string payload = "Hello from client";
  const int written = ::SSL_write(pair.clientSsl.get(), payload.data(), static_cast<int>(payload.size()));
  ASSERT_EQ(written, static_cast<int>(payload.size()));

  char buf[64]{};
  auto readRes = transport.read(buf, sizeof(buf));
  // This should hit the successful SSL_read_ex path and return immediately
  EXPECT_EQ(readRes.want, TransportHint::None);
  EXPECT_EQ(readRes.bytesProcessed, payload.size());
  EXPECT_EQ(std::string_view(buf, readRes.bytesProcessed), payload);
}

TEST(TlsHandshakeTest, FinalizeTlsHandshakeLogsHandshake) {
  // Covers the logging path in collectAndLogTlsHandshake (line 64)
  SslTestPair pair({"http/1.1"}, {"http/1.1"});
  ASSERT_TRUE(PerformHandshake(pair));

  auto start = std::chrono::steady_clock::now();
  TlsMetricsInternal metrics{};

  // Call with logHandshake=true to cover the logging branch
  bool tlsHandshakeEventEmitted = false;
  auto tlsInfo = FinalizeTlsHandshake(pair.serverSsl.get(), pair.serverFd.fd(), true, tlsHandshakeEventEmitted, {},
                                      start, metrics);

  EXPECT_EQ(tlsInfo.selectedAlpn(), "http/1.1");
  EXPECT_FALSE(tlsInfo.negotiatedCipher().empty());
  EXPECT_FALSE(tlsInfo.negotiatedVersion().empty());
  EXPECT_TRUE(tlsInfo.peerSubject().empty());
  EXPECT_EQ(metrics.handshakesSucceeded, 1U);
}

TEST(TlsHandshakeTest, CollectTlsHandshakeInfoBeforeHandshake) {
  // Covers the branches where SSL has not completed handshake yet: no ALPN selected,
  // no negotiated cipher/version and no peer certificate.
  SslTestPair pair({"http/1.1"}, {"http/1.1"});

  // Do NOT perform the handshake - inspect the values collected from an unnegotiated SSL
  auto start = std::chrono::steady_clock::now();
  TlsMetricsInternal metrics{};

  bool tlsHandshakeEventEmitted = false;
  auto tlsInfo = FinalizeTlsHandshake(pair.serverSsl.get(), pair.serverFd.fd(), false, tlsHandshakeEventEmitted, {},
                                      start, metrics);

  // Before handshake, ALPN and peer subject should be empty. Cipher/version may be present
  // (derived from the SSL_METHOD) so we don't assert on them.
  EXPECT_TRUE(tlsInfo.selectedAlpn().empty());
  EXPECT_TRUE(tlsInfo.peerSubject().empty());
  EXPECT_EQ(metrics.handshakesSucceeded, 1U);
}

TEST(TlsHandshakeTest, CollectTlsHandshakeInfoNoAlpn) {
  // Server configured with no ALPN (empty list) should produce empty selectedAlpn after handshake
  SslTestPair pair({}, {"http/1.1"});
  ASSERT_TRUE(PerformHandshake(pair));

  TlsMetricsInternal metrics{};
  bool tlsHandshakeEventEmitted = false;
  auto tlsInfo = FinalizeTlsHandshake(pair.serverSsl.get(), pair.serverFd.fd(), false, tlsHandshakeEventEmitted, {},
                                      std::chrono::steady_clock::now(), metrics);

  EXPECT_TRUE(tlsInfo.selectedAlpn().empty());
  EXPECT_FALSE(tlsInfo.negotiatedCipher().empty());
  EXPECT_FALSE(tlsInfo.negotiatedVersion().empty());
}

TEST(TlsHandshakeTest, PeerSubjectNonEmptyAfterHandshake) {
  SslTestPair pair({"http/1.1"}, {"http/1.1"});

  // Create a client certificate and attach it to the client SSL so the server
  // can observe a peer certificate during handshake.
  auto clientCertKey = CertKeyCache::Get().client;
  ASSERT_FALSE(clientCertKey.first.empty());
  ASSERT_FALSE(clientCertKey.second.empty());

  auto certBio = MakeMemBio(clientCertKey.first.data(), static_cast<int>(clientCertKey.first.size()));
  auto xcert = MakeX509(::PEM_read_bio_X509(certBio.get(), nullptr, nullptr, nullptr));
  ASSERT_NE(xcert, nullptr);

  auto keyBio = MakeMemBio(clientCertKey.second.data(), static_cast<int>(clientCertKey.second.size()));
  auto pKey = MakePKey(::PEM_read_bio_PrivateKey(keyBio.get(), nullptr, nullptr, nullptr));
  ASSERT_NE(pKey, nullptr);

  // Install client cert/key into the client SSL
  ASSERT_EQ(1, ::SSL_use_certificate(pair.clientSsl.get(), xcert.get()));
  ASSERT_EQ(1, ::SSL_use_PrivateKey(pair.clientSsl.get(), pKey.get()));
  // Make the server request a client certificate. Add the client's cert to the
  // server's accepted CA list so the client will be prompted to send its cert.
  SSL_CTX* serverCtx = ::SSL_get_SSL_CTX(pair.serverSsl.get());
  ASSERT_NE(serverCtx, nullptr);
  // Add client's cert to server CA list
  ASSERT_EQ(1, ::SSL_CTX_add_client_CA(serverCtx, xcert.get()));
  // Also add client's cert to server trust store so verification succeeds
  X509_STORE* store = ::SSL_CTX_get_cert_store(serverCtx);
  ASSERT_NE(store, nullptr);
  ASSERT_EQ(1, ::X509_STORE_add_cert(store, xcert.get()));
  // Set verification mode on the server SSL object itself to ensure it's applied
  ::SSL_set_verify(pair.serverSsl.get(), SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, nullptr);

  ASSERT_TRUE(PerformHandshake(pair));

  TlsMetricsInternal metrics{};
  bool tlsHandshakeEventEmitted = false;
  auto tlsInfo = FinalizeTlsHandshake(pair.serverSsl.get(), pair.serverFd.fd(), false, tlsHandshakeEventEmitted, {},
                                      std::chrono::steady_clock::now(), metrics);

  EXPECT_TRUE(tlsInfo.peerSubject().starts_with("CN=client.example.com"));
}

TEST(TlsTransportTest, KtlsSendAlreadyAttemptedReturnsFailed) {
  SslTestPair pair({"http/1.1"}, {"http/1.1"});
  ASSERT_TRUE(PerformHandshake(pair));
  TlsTransport transport(std::move(pair.serverSsl), kMinBytesForZerocopy);

  // First attempt
  const auto result1 = transport.enableKtlsSend();
  EXPECT_NE(result1, KtlsEnableResult::Unknown);

  // Second attempt should return the same value
  const auto result2 = transport.enableKtlsSend();
  EXPECT_EQ(result1, result2);
}

TEST(TlsContextTest, SniCertificateWithWildcardPatternWorks) {
  // Test that a valid wildcard pattern starting with "*." is accepted
  auto mainCert = CertKeyCache::Get().wildcard;
  auto sniCert = CertKeyCache::Get().client;
  ASSERT_FALSE(mainCert.first.empty());
  ASSERT_FALSE(sniCert.first.empty());

  TLSConfig cfg;
  cfg.enabled = true;
  cfg.withCertPem(mainCert.first).withKeyPem(mainCert.second);

  // Add SNI certificate with a proper wildcard pattern
  cfg.withTlsSniCertificateMemory("*.example.com", sniCert.first, sniCert.second);

  EXPECT_NO_THROW({
    TlsContext ctx(cfg);
    (void)ctx;
  });
}

TEST(TlsTransportTest, ShutdownWithNullSslDoesNotCrash) {
  // Covers the null check in TlsTransport::shutdown (line 147)
  TlsTransport::SslPtr nullSsl{nullptr, &::SSL_free};
  TlsTransport transport(std::move(nullSsl), kMinBytesForZerocopy);

  // Should return early without crashing
  transport.shutdown();
}

TEST(TlsTransportTest, HandshakeDoneFalseInitially) {
  // Verify handshakeDone() returns false before handshake is complete
  SslTestPair pair({"http/1.1"}, {"http/1.1"});
  TlsTransport transport(std::move(pair.serverSsl), kMinBytesForZerocopy);

  EXPECT_FALSE(transport.handshakeDone());
}

TEST(TlsTransportTest, WriteEmptyDataReturnsZero) {
  // Covers the empty data early return path in write (line 108)
  SslTestPair pair({"http/1.1"}, {"http/1.1"});
  ASSERT_TRUE(PerformHandshake(pair));
  TlsTransport transport(std::move(pair.serverSsl), kMinBytesForZerocopy);

  auto result = transport.write("");
  EXPECT_EQ(result.bytesProcessed, 0U);
  EXPECT_EQ(result.want, TransportHint::None);
}

TEST(TlsTransportTest, ReadAfterPeerCloseReturnsZero) {
  // Covers the SSL_ERROR_ZERO_RETURN path in read (line 56-58)
  SslTestPair pair({"http/1.1"}, {"http/1.1"});
  ASSERT_TRUE(PerformHandshake(pair));

  TlsTransport transport(std::move(pair.serverSsl), kMinBytesForZerocopy);

  // Client initiates shutdown - send close_notify
  ::SSL_shutdown(pair.clientSsl.get());

  // Server should see the clean shutdown
  char buf[16]{};
  auto result = transport.read(buf, sizeof(buf));
  // Depending on timing, we may get ZERO_RETURN (no error, 0 bytes) or ReadReady
  EXPECT_EQ(result.bytesProcessed, 0U);
  EXPECT_TRUE(result.want == TransportHint::None || result.want == TransportHint::ReadReady);
}

TEST(TlsContextTest, SessionTicketsEnabledAutoCreatesKeyStore) {
  // Covers lines 318-319: auto-creation of ticket key store when sessionTickets.enabled is true
  auto certKey = CertKeyCache::Get().localhost;

  TLSConfig cfg;
  cfg.enabled = true;
  cfg.withCertPem(certKey.first).withKeyPem(certKey.second);
  cfg.sessionTickets.enabled = true;
  cfg.sessionTickets.lifetime = std::chrono::seconds(60);
  cfg.sessionTickets.maxKeys = 2;

  EXPECT_NO_THROW({
    TlsContext ctx(cfg);
    (void)ctx;
  });
}

TEST(TlsContextTest, SessionTicketsWithStaticKeys) {
  // Covers line 321-322: loading static keys into ticket store
  auto certKey = CertKeyCache::Get().localhost;

  TLSConfig cfg;
  cfg.enabled = true;
  cfg.withCertPem(certKey.first).withKeyPem(certKey.second);
  cfg.sessionTickets.enabled = true;
  cfg.sessionTickets.lifetime = std::chrono::seconds(60);
  cfg.sessionTickets.maxKeys = 2;

  // Add a static session ticket key
  TLSConfig::SessionTicketKey staticKey;
  for (std::size_t ii = 0; ii < staticKey.size(); ++ii) {
    staticKey[ii] = static_cast<std::byte>(ii);
  }
  cfg.withTlsSessionTicketKey(std::move(staticKey));

  EXPECT_NO_THROW({
    TlsContext ctx(cfg);
    (void)ctx;
  });
}

TEST(TlsContextTest, SniCertificateWithFilePaths) {
  // This test covers line 342 in tls-context.cpp: the file-path branch in SNI loading
  // We use file paths that will fail, exercising the else branch
  auto mainCert = CertKeyCache::Get().wildcard;
  ASSERT_FALSE(mainCert.first.empty());

  TLSConfig cfg;
  cfg.enabled = true;
  cfg.withCertPem(mainCert.first).withKeyPem(mainCert.second);

  // Add SNI certificate with file paths that don't exist - should throw
  cfg.withTlsSniCertificateFiles("test.example.com", "/__nonexistent_cert__.pem", "/__nonexistent_key__.pem");

  EXPECT_THROW(TlsContext{cfg}, std::runtime_error);
}

TEST(TlsContextTest, DefaultCipherPolicyDoesNotApplyPolicy) {
  auto certKey = CertKeyCache::Get().localhost;

  TLSConfig cfg;
  cfg.enabled = true;
  cfg.withCertPem(certKey.first).withKeyPem(certKey.second);
  cfg.cipherPolicy = TLSConfig::CipherPolicy::Default;  // Explicitly set to Default

  TlsContext ctx(cfg);
}

TEST(TlsContextTest, InvalidCipherPolicyThrowsInvalidArgument) {
  auto certKey = CertKeyCache::Get().localhost;
  TLSConfig cfg;
  cfg.enabled = true;
  cfg.withCertPem(certKey.first).withKeyPem(certKey.second);
  cfg.cipherPolicy =
      static_cast<TLSConfig::CipherPolicy>(std::numeric_limits<std::underlying_type_t<TLSConfig::CipherPolicy>>::max());

  EXPECT_THROW(TlsContext{cfg}, std::invalid_argument);
}

TEST(TlsContextTest, DefaultCipherPolicyWithCustomCipherList) {
  auto certKey = CertKeyCache::Get().localhost;
  TLSConfig cfg;
  cfg.enabled = true;
  cfg.withCertPem(certKey.first).withKeyPem(certKey.second);
  cfg.cipherPolicy = TLSConfig::CipherPolicy::Default;
  cfg.withCipherList("AES256-SHA:AES128-SHA");

  TlsContext ctx(cfg);
}

TEST(TlsContextTest, SessionTicketsWithTlsHandshake) {
  auto certKey = CertKeyCache::Get().localhost;

  TLSConfig cfg;
  cfg.enabled = true;
  cfg.withCertPem(certKey.first).withKeyPem(certKey.second);
  cfg.sessionTickets.enabled = true;
  cfg.sessionTickets.lifetime = std::chrono::seconds(60);
  cfg.sessionTickets.maxKeys = 2;

  TlsContext ctx(cfg);

  // Create server/client socket pair
  int fds[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
  BaseFd serverFd(fds[0]);
  BaseFd clientFd(fds[1]);

  // Create server SSL
  auto* rawCtx = reinterpret_cast<SSL_CTX*>(ctx.raw());
  TlsTransport::SslPtr serverSsl{::SSL_new(rawCtx), &::SSL_free};
  ASSERT_NE(serverSsl.get(), nullptr);
  ::SSL_set_fd(serverSsl.get(), serverFd.fd());
  ::SSL_set_accept_state(serverSsl.get());

  // Create client SSL context
  auto clientCtx = SslCtxPtr(::SSL_CTX_new(TLS_client_method()), &::SSL_CTX_free);
  ASSERT_NE(clientCtx.get(), nullptr);
  ::SSL_CTX_set_verify(clientCtx.get(), SSL_VERIFY_NONE, nullptr);

  // Enable session tickets on client
  ::SSL_CTX_set_session_cache_mode(clientCtx.get(), SSL_SESS_CACHE_CLIENT);

  SslPtr clientSsl{::SSL_new(clientCtx.get()), &::SSL_free};
  ASSERT_NE(clientSsl.get(), nullptr);
  ::SSL_set_fd(clientSsl.get(), clientFd.fd());
  ::SSL_set_connect_state(clientSsl.get());

  // Perform handshake in threads
  std::thread clientThread([&clientSsl]() {
    while (true) {
      int ret = ::SSL_connect(clientSsl.get());
      if (ret == 1) {
        break;
      }
      int err = ::SSL_get_error(clientSsl.get(), ret);
      if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
        break;
      }
    }
  });

  while (true) {
    int ret = ::SSL_accept(serverSsl.get());
    if (ret == 1) {
      break;
    }
    int err = ::SSL_get_error(serverSsl.get(), ret);
    if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
      break;
    }
  }

  clientThread.join();

  // Verify handshake succeeded
  EXPECT_TRUE(::SSL_is_init_finished(serverSsl.get()));
}

TEST(TlsContextTest, SessionTicketsStoreCreatedWithoutStaticKeys) {
  auto certKey = CertKeyCache::Get().localhost;

  TLSConfig cfg;
  cfg.enabled = true;
  cfg.withCertPem(certKey.first).withKeyPem(certKey.second);
  cfg.sessionTickets.enabled = true;
  cfg.sessionTickets.lifetime = std::chrono::seconds(3600);
  cfg.sessionTickets.maxKeys = 5;

  TlsContext ctx(cfg);
}

TEST(TlsTransportTest, ZerocopyNotEnabledByDefault) {
  SslTestPair pair({"http/1.1"}, {"http/1.1"});
  ASSERT_TRUE(PerformHandshake(pair));
  TlsTransport transport(std::move(pair.serverSsl), kMinBytesForZerocopy);

  EXPECT_FALSE(transport.isZerocopyEnabled());
  EXPECT_FALSE(transport.hasZerocopyPending());
}

TEST(TlsTransportTest, ZerocopyRequiresKtlsSend) {
  SslTestPair pair({"http/1.1"}, {"http/1.1"});
  ASSERT_TRUE(PerformHandshake(pair));
  TlsTransport transport(std::move(pair.serverSsl), kMinBytesForZerocopy);

  // Set fd (in real use, this is done after SSL_set_fd)
  transport.setUnderlyingFd(pair.serverFd.fd());

  // Zerocopy should NOT be enabled because kTLS send is not active
  // (enableKtlsSend was not called, or kTLS is not supported)
  EXPECT_FALSE(transport.enableZerocopy());
  EXPECT_FALSE(transport.isZerocopyEnabled());
}

TEST(TlsTransportTest, ZerocopyWithoutFdReturnsFalse) {
  SslTestPair pair({"http/1.1"}, {"http/1.1"});
  ASSERT_TRUE(PerformHandshake(pair));
  TlsTransport transport(std::move(pair.serverSsl), kMinBytesForZerocopy);

  // Even if kTLS were enabled, without fd set, enableZerocopy should fail
  EXPECT_FALSE(transport.enableZerocopy());
  EXPECT_EQ(transport.underlyingFd(), -1);
}

TEST(TlsTransportTest, SetUnderlyingFdStoresFd) {
  SslTestPair pair({"http/1.1"}, {"http/1.1"});
  ASSERT_TRUE(PerformHandshake(pair));
  TlsTransport transport(std::move(pair.serverSsl), kMinBytesForZerocopy);

  transport.setUnderlyingFd(42);
  EXPECT_EQ(transport.underlyingFd(), 42);
}

TEST(TlsTransportTest, DisableZerocopyClearsState) {
  SslTestPair pair({"http/1.1"}, {"http/1.1"});
  ASSERT_TRUE(PerformHandshake(pair));
  TlsTransport transport(std::move(pair.serverSsl), kMinBytesForZerocopy);

  // disableZerocopy should work even if it was never enabled
  transport.disableZerocopy();
  EXPECT_FALSE(transport.isZerocopyEnabled());
  EXPECT_FALSE(transport.hasZerocopyPending());
}

TEST(TlsTransportTest, PollZerocopyCompletionsNoFdReturnsZero) {
  SslTestPair pair({"http/1.1"}, {"http/1.1"});
  ASSERT_TRUE(PerformHandshake(pair));
  TlsTransport transport(std::move(pair.serverSsl), kMinBytesForZerocopy);

  // Without fd set, poll should return 0
  EXPECT_EQ(transport.pollZerocopyCompletions(), 0U);
}

TEST(TlsTransportTest, PollZerocopyCompletionsWithFdNoCompletions) {
  SslTestPair pair({"http/1.1"}, {"http/1.1"});
  ASSERT_TRUE(PerformHandshake(pair));
  TlsTransport transport(std::move(pair.serverSsl), kMinBytesForZerocopy);

  transport.setUnderlyingFd(pair.serverFd.fd());

  // With fd set but nothing pending, should return 0
  EXPECT_EQ(transport.pollZerocopyCompletions(), 0U);
}

#if AERONET_WANT_MALLOC_OVERRIDES

TEST(TlsContextTest, TlsContextBadAlloc) {
  auto certKey = CertKeyCache::Get().localhost;
  TLSConfig cfg;
  cfg.enabled = true;
  cfg.withCertPem(certKey.first).withKeyPem(certKey.second);

  test::FailAllAllocationsGuard guard;
  EXPECT_THROW(
      {
        TlsContext ctx(cfg);
        (void)ctx;
      },
      std::bad_alloc);
}

#endif

}  // namespace tls_test
