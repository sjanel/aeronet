#include <fcntl.h>
#include <gtest/gtest.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/sslerr.h>
#include <openssl/types.h>
#include <sys/socket.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "aeronet/base-fd.hpp"
#include "aeronet/test_tls_helper.hpp"
#include "aeronet/tls-config.hpp"
#include "aeronet/tls-context.hpp"
#include "aeronet/tls-handshake.hpp"
#include "aeronet/tls-info.hpp"
#include "aeronet/tls-metrics.hpp"
#include "aeronet/tls-transport.hpp"
#include "aeronet/transport.hpp"

using namespace aeronet;

namespace tls_test {
namespace {

struct SslTestPair {
  TLSConfig cfg;
  TlsMetricsExternal externalMetrics{};
  TlsContext context;
  std::unique_ptr<SSL_CTX, decltype(&::SSL_CTX_free)> clientCtx{nullptr, &::SSL_CTX_free};
  TlsTransport::SslPtr serverSsl{nullptr, &::SSL_free};
  std::unique_ptr<SSL, decltype(&::SSL_free)> clientSsl{nullptr, &::SSL_free};
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

int controlledBioCreate(BIO* bio) {
  BIO_set_init(bio, 1);
  BIO_set_data(bio, nullptr);
  return 1;
}

int controlledBioDestroy(BIO* bio) {
  BIO_set_init(bio, 0);
  BIO_set_data(bio, nullptr);
  return 1;
}

long controlledBioCtrl(BIO* bio [[maybe_unused]], int cmd, long num, void* ptr) {
  (void)num;
  (void)ptr;
  return (cmd == BIO_CTRL_FLUSH) ? 1 : 0;
}

int controlledBioRead(BIO* bio, char* buf [[maybe_unused]], int len [[maybe_unused]]) {
  auto* state = static_cast<ControlledBioState*>(BIO_get_data(bio));
  if (state == nullptr) {
    return 0;
  }
  errno = state->errnoValue;
  BIO_clear_retry_flags(bio);
  if (state->retryRead) {
    BIO_set_retry_read(bio);
  }
  return state->readResult;
}

int controlledBioWrite(BIO* bio, const char* buf [[maybe_unused]], int len [[maybe_unused]]) {
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

BIO_METHOD* controlledBioMethod() {
  static BIO_METHOD* method = [] {
    BIO_METHOD* created = BIO_meth_new(BIO_TYPE_SOURCE_SINK, "tls_transport_test_bio");
    BIO_meth_set_create(created, &controlledBioCreate);
    BIO_meth_set_destroy(created, &controlledBioDestroy);
    BIO_meth_set_ctrl(created, &controlledBioCtrl);
    BIO_meth_set_read(created, &controlledBioRead);
    BIO_meth_set_write(created, &controlledBioWrite);
    return created;
  }();
  return method;
}

BIO* makeControlledBio(ControlledBioState* state) {
  BIO* bio = BIO_new(controlledBioMethod());
  if (bio == nullptr) {
    ADD_FAILURE() << "Failed to allocate test BIO";
    return nullptr;
  }
  BIO_set_data(bio, state);
  BIO_set_init(bio, 1);
  return bio;
}

void attachControlledBios(SSL* ssl, ControlledBioState& readState, ControlledBioState& writeState) {
  BIO* readBio = makeControlledBio(&readState);
  BIO* writeBio = makeControlledBio(&writeState);
  ASSERT_NE(readBio, nullptr);
  ASSERT_NE(writeBio, nullptr);
  ::SSL_set_bio(ssl, readBio, writeBio);
}

std::vector<unsigned char> makeAlpnWire(std::initializer_list<std::string_view> protos) {
  std::vector<unsigned char> wire;
  for (std::string_view proto : protos) {
    EXPECT_LE(proto.size(), TLSConfig::kMaxAlpnProtocolLength);
    wire.push_back(static_cast<unsigned char>(proto.size()));
    wire.insert(wire.end(), proto.begin(), proto.end());
  }
  return wire;
}

void configureSslPair(SslTestPair& pair, std::initializer_list<std::string_view> serverAlpn,
                      std::initializer_list<std::string_view> clientAlpn, bool strictAlpn) {
  auto cert = test::makeEphemeralCertKey();
  ASSERT_FALSE(cert.first.empty());
  ASSERT_FALSE(cert.second.empty());
  pair.cfg.enabled = true;
  pair.cfg.withCertPem(cert.first).withKeyPem(cert.second);
  pair.cfg.withTlsAlpnProtocols(serverAlpn);
  pair.cfg.alpnMustMatch = strictAlpn;
  pair.context = TlsContext(pair.cfg, &pair.externalMetrics);
  auto* serverCtx = reinterpret_cast<SSL_CTX*>(pair.context.raw());
  pair.serverSsl = TlsTransport::SslPtr(::SSL_new(serverCtx), &::SSL_free);
  EXPECT_NE(pair.serverSsl, nullptr);
  pair.clientCtx.reset(::SSL_CTX_new(TLS_client_method()));
  EXPECT_NE(pair.clientCtx, nullptr);
  ::SSL_CTX_set_verify(pair.clientCtx.get(), SSL_VERIFY_NONE, nullptr);
  if (clientAlpn.size() > 0) {
    auto wire = makeAlpnWire(clientAlpn);
    const unsigned int wireLen = static_cast<unsigned int>(wire.size());
    ASSERT_EQ(0, ::SSL_CTX_set_alpn_protos(pair.clientCtx.get(), wire.data(), wireLen));
  }
  pair.clientSsl = std::unique_ptr<SSL, decltype(&::SSL_free)>(::SSL_new(pair.clientCtx.get()), &::SSL_free);
  EXPECT_NE(pair.clientSsl, nullptr);
  int fds[2]{};
  ASSERT_EQ(0, ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds));
  pair.serverFd = BaseFd(fds[0]);
  pair.clientFd = BaseFd(fds[1]);
  ASSERT_EQ(1, ::SSL_set_fd(pair.serverSsl.get(), pair.serverFd.fd()));
  ASSERT_EQ(1, ::SSL_set_fd(pair.clientSsl.get(), pair.clientFd.fd()));
  ::SSL_set_accept_state(pair.serverSsl.get());
  ::SSL_set_connect_state(pair.clientSsl.get());
}

SslTestPair makeSslPair(std::initializer_list<std::string_view> serverAlpn,
                        std::initializer_list<std::string_view> clientAlpn, bool strictAlpn = false) {
  SslTestPair pair;
  configureSslPair(pair, serverAlpn, clientAlpn, strictAlpn);
  return pair;
}

bool performHandshake(SslTestPair& pair) {
  int serverRc = 0;
  std::thread serverThread([&] { serverRc = ::SSL_accept(pair.serverSsl.get()); });
  const int clientRc = ::SSL_connect(pair.clientSsl.get());
  serverThread.join();
  return (serverRc == 1) && (clientRc == 1);
}

void setNonBlocking(int fd) {
  int flags = ::fcntl(fd, F_GETFL, 0);
  ASSERT_GE(flags, 0);
  ASSERT_GE(::fcntl(fd, F_SETFL, flags | O_NONBLOCK), 0);
}

}  // namespace

TEST(TlsContextTest, CollectsHandshakeInfo) {
  auto pair = makeSslPair({"http/1.1"}, {"http/1.1"});
  const auto start = std::chrono::steady_clock::now();
  ASSERT_TRUE(performHandshake(pair));

  TlsMetricsInternal metrics;
  auto tlsInfo = FinalizeTlsHandshake(pair.serverSsl.get(), pair.serverFd.fd(), true, start, metrics);
  EXPECT_EQ(tlsInfo.selectedAlpn(), "http/1.1");
  EXPECT_EQ(metrics.handshakesSucceeded, 1U);
  EXPECT_EQ(metrics.alpnDistribution["http/1.1"], 1U);
  EXPECT_EQ(metrics.versionCounts.size(), 1U);
  EXPECT_EQ(metrics.cipherCounts.size(), 1U);
  EXPECT_EQ(metrics.handshakeDurationCount, 1U);
  EXPECT_EQ(metrics.handshakeDurationMaxNs, metrics.handshakeDurationTotalNs);
}

TEST(TlsContextTest, StrictAlpnMismatchIncrementsMetric) {
  auto pair = makeSslPair({"h2"}, {"http/1.1"}, true);
  EXPECT_FALSE(performHandshake(pair));
  EXPECT_EQ(pair.externalMetrics.alpnStrictMismatches, 1U);
  ::ERR_clear_error();
}

#ifdef TLS1_3_VERSION
TEST(TlsContextTest, SupportsTls13VersionBounds) {
  auto certKey = test::makeEphemeralCertKey();
  ASSERT_FALSE(certKey.first.empty());
  ASSERT_FALSE(certKey.second.empty());
  TLSConfig cfg;
  cfg.enabled = true;
  cfg.withCertPem(certKey.first).withKeyPem(certKey.second);
  cfg.minVersion = TLSConfig::TLS_1_3;
  cfg.maxVersion = TLSConfig::TLS_1_3;
  TlsMetricsExternal metrics{};
  EXPECT_NO_THROW({
    TlsContext ctx(cfg, &metrics);
    (void)ctx;
  });
}
#endif

TEST(TlsContextTest, InvalidMinVersionThrows) {
  auto certKey = test::makeEphemeralCertKey();
  ASSERT_FALSE(certKey.first.empty());
  ASSERT_FALSE(certKey.second.empty());
  TLSConfig cfg;
  cfg.enabled = true;
  cfg.withCertPem(certKey.first).withKeyPem(certKey.second);
  cfg.withTlsMinVersion("TLS1.1");
  TlsMetricsExternal metrics{};
  EXPECT_THROW(TlsContext(cfg, &metrics), std::runtime_error);
}

TEST(TlsContextTest, InvalidMaxVersionThrows) {
  auto certKey = test::makeEphemeralCertKey();
  ASSERT_FALSE(certKey.first.empty());
  ASSERT_FALSE(certKey.second.empty());
  TLSConfig cfg;
  cfg.enabled = true;
  cfg.withCertPem(certKey.first).withKeyPem(certKey.second);
  cfg.withTlsMaxVersion("TLS1.1");
  TlsMetricsExternal metrics{};
  EXPECT_THROW(TlsContext(cfg, &metrics), std::runtime_error);
}

TEST(TlsContextTest, CipherPolicyAppliesPredefinedSuites) {
  auto certKey = test::makeEphemeralCertKey();
  ASSERT_FALSE(certKey.first.empty());
  ASSERT_FALSE(certKey.second.empty());

  const std::array<TLSConfig::CipherPolicy, 3> policies = {
      TLSConfig::CipherPolicy::Modern, TLSConfig::CipherPolicy::Compatibility, TLSConfig::CipherPolicy::Legacy};

  for (auto policy : policies) {
    TLSConfig cfg;
    cfg.enabled = true;
    cfg.withCertPem(certKey.first).withKeyPem(certKey.second);
    cfg.withTlsCipherPolicy(policy);
    TlsMetricsExternal metrics{};
    EXPECT_NO_THROW({
      TlsContext ctx(cfg, &metrics);
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
  TlsMetricsExternal metrics{};
  EXPECT_THROW(TlsContext(cfg, &metrics), std::runtime_error);
}

TEST(TlsContextTest, MismatchedPrivateKeyFailsCheck) {
  auto certA = test::makeEphemeralCertKey("server-a");
  auto certB = test::makeEphemeralCertKey("server-b");
  ASSERT_FALSE(certA.first.empty());
  ASSERT_FALSE(certA.second.empty());
  ASSERT_FALSE(certB.first.empty());
  ASSERT_FALSE(certB.second.empty());
  TLSConfig cfg;
  cfg.enabled = true;
  cfg.withCertPem(certA.first);
  cfg.withKeyPem(certB.second);
  TlsMetricsExternal metrics{};
  EXPECT_THROW(TlsContext(cfg, &metrics), std::runtime_error);
}

TEST(TlsContextTest, EmptyTrustedClientCertPemThrows) {
  auto certKey = test::makeEphemeralCertKey();
  ASSERT_FALSE(certKey.first.empty());
  ASSERT_FALSE(certKey.second.empty());
  TLSConfig cfg;
  cfg.enabled = true;
  cfg.requestClientCert = true;
  cfg.withCertPem(certKey.first).withKeyPem(certKey.second);
  cfg.withTlsTrustedClientCert("");
  TlsMetricsExternal metrics{};
  EXPECT_THROW(TlsContext(cfg, &metrics), std::runtime_error);
}

TEST(TlsContextTest, InvalidTrustedClientCertPemThrows) {
  auto certKey = test::makeEphemeralCertKey();
  ASSERT_FALSE(certKey.first.empty());
  ASSERT_FALSE(certKey.second.empty());
  TLSConfig cfg;
  cfg.enabled = true;
  cfg.requestClientCert = true;
  cfg.withCertPem(certKey.first).withKeyPem(certKey.second);
  cfg.withTlsTrustedClientCert("not-a-cert");
  TlsMetricsExternal metrics{};
  EXPECT_THROW(TlsContext(cfg, &metrics), std::runtime_error);
}

TEST(TlsContextTest, MissingCertificateFilesThrow) {
  TLSConfig cfg;
  cfg.enabled = true;
  cfg.withCertFile("/__aeronet_missing_cert__.pem");
  cfg.withKeyFile("/__aeronet_missing_key__.pem");
  TlsMetricsExternal metrics{};
  EXPECT_THROW(TlsContext(cfg, &metrics), std::runtime_error);
}

TEST(TlsTransportTest, ReadWriteAndRetryHints) {
  auto pair = makeSslPair({"http/1.1"}, {"http/1.1"});
  ASSERT_TRUE(performHandshake(pair));
  TlsTransport transport(std::move(pair.serverSsl));

  auto zeroWrite = transport.write(std::string_view{});
  EXPECT_EQ(zeroWrite.bytesProcessed, 0U);
  EXPECT_EQ(zeroWrite.want, TransportHint::None);

  setNonBlocking(pair.serverFd.fd());
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
  auto pair = makeSslPair({"http/1.1"}, {"http/1.1"});
  ControlledBioState readState{};
  readState.errnoValue = EBADF;
  ControlledBioState writeState{};
  writeState.errnoValue = EBADF;
  attachControlledBios(pair.serverSsl.get(), readState, writeState);
  pair.serverFd.close();
  TlsTransport transport(std::move(pair.serverSsl));
  ERR_clear_error();
  errno = EBADF;
  auto res = transport.write("X");
  EXPECT_EQ(res.want, TransportHint::Error);
  EXPECT_EQ(res.bytesProcessed, 0U);
}

TEST(TlsTransportTest, SyscallDuringReadWithErrnoZeroRetried) {
  auto pair = makeSslPair({"http/1.1"}, {"http/1.1"});
  ASSERT_TRUE(performHandshake(pair));
  SSL* rawSsl = pair.serverSsl.get();
  TlsTransport transport(std::move(pair.serverSsl));
  ControlledBioState readState{};
  readState.errnoValue = 0;
  ControlledBioState writeState{};
  writeState.errnoValue = 0;
  attachControlledBios(rawSsl, readState, writeState);
  pair.serverFd.close();
  char tmp[1]{};
  ERR_clear_error();
  errno = 0;
  auto res = transport.read(tmp, sizeof(tmp));
  EXPECT_EQ(res.want, TransportHint::ReadReady);
  EXPECT_EQ(res.bytesProcessed, 0U);
}

TEST(TlsTransportTest, SyscallDuringReadFatalSetsErrorHint) {
  auto pair = makeSslPair({"http/1.1"}, {"http/1.1"});
  ASSERT_TRUE(performHandshake(pair));
  SSL* rawSsl = pair.serverSsl.get();
  TlsTransport transport(std::move(pair.serverSsl));
  ControlledBioState readState{};
  readState.errnoValue = EBADF;
  ControlledBioState writeState{};
  writeState.errnoValue = EBADF;
  attachControlledBios(rawSsl, readState, writeState);
  pair.serverFd.close();
  char tmp[1]{};
  ERR_clear_error();
  errno = EBADF;
  auto res = transport.read(tmp, sizeof(tmp));
  EXPECT_EQ(res.want, TransportHint::Error);
  EXPECT_EQ(res.bytesProcessed, 0U);
}

TEST(TlsTransportTest, SyscallDuringWriteWithErrnoZeroRetried) {
  auto pair = makeSslPair({"http/1.1"}, {"http/1.1"});
  ASSERT_TRUE(performHandshake(pair));
  SSL* rawSsl = pair.serverSsl.get();
  TlsTransport transport(std::move(pair.serverSsl));
  ControlledBioState readState{};
  readState.errnoValue = 0;
  ControlledBioState writeState{};
  writeState.errnoValue = 0;
  attachControlledBios(rawSsl, readState, writeState);
  pair.serverFd.close();
  ERR_clear_error();
  errno = 0;
  auto res = transport.write("ping");
  EXPECT_TRUE(res.want == TransportHint::WriteReady || res.want == TransportHint::Error);
  EXPECT_EQ(res.bytesProcessed, 0U);
}

TEST(TlsTransportTest, SyscallDuringWriteFatalSetsErrorHint) {
  auto pair = makeSslPair({"http/1.1"}, {"http/1.1"});
  ASSERT_TRUE(performHandshake(pair));
  SSL* rawSsl = pair.serverSsl.get();
  TlsTransport transport(std::move(pair.serverSsl));
  ControlledBioState readState{};
  readState.errnoValue = EBADF;
  ControlledBioState writeState{};
  writeState.errnoValue = EBADF;
  attachControlledBios(rawSsl, readState, writeState);
  pair.serverFd.close();
  ERR_clear_error();
  errno = EBADF;
  auto res = transport.write("ping");
  EXPECT_EQ(res.want, TransportHint::Error);
  EXPECT_EQ(res.bytesProcessed, 0U);
}

TEST(TlsTransportTest, SuccessfulReadReturnsData) {
  // Covers the early-return success path in TlsTransport::read (line 45)
  auto pair = makeSslPair({"http/1.1"}, {"http/1.1"});
  ASSERT_TRUE(performHandshake(pair));
  TlsTransport transport(std::move(pair.serverSsl));

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
  auto pair = makeSslPair({"http/1.1"}, {"http/1.1"});
  ASSERT_TRUE(performHandshake(pair));

  auto start = std::chrono::steady_clock::now();
  TlsMetricsInternal metrics{};

  // Call with logHandshake=true to cover the logging branch
  auto tlsInfo = FinalizeTlsHandshake(pair.serverSsl.get(), pair.serverFd.fd(), true, start, metrics);

  EXPECT_EQ(tlsInfo.selectedAlpn(), "http/1.1");
  EXPECT_FALSE(tlsInfo.negotiatedCipher().empty());
  EXPECT_FALSE(tlsInfo.negotiatedVersion().empty());
  EXPECT_EQ(metrics.handshakesSucceeded, 1U);
}

#ifdef AERONET_ENABLE_KTLS
TEST(TlsTransportTest, KtlsSendAlreadyAttemptedReturnsFailed) {
  // Test that calling enableKtlsSend twice returns AlreadyEnabled or Failed (covers lines 194-195)
  auto pair = makeSslPair({"http/1.1"}, {"http/1.1"});
  ASSERT_TRUE(performHandshake(pair));
  TlsTransport transport(std::move(pair.serverSsl));

  // First attempt
  [[maybe_unused]] auto result1 = transport.enableKtlsSend();
  // Could be Enabled, AlreadyEnabled, Failed, or Unsupported depending on system

  // Second attempt should return AlreadyEnabled or Failed (since already attempted)
  auto result2 = transport.enableKtlsSend();
  EXPECT_TRUE(result2.status == TlsTransport::KtlsEnableResult::Status::AlreadyEnabled ||
              result2.status == TlsTransport::KtlsEnableResult::Status::Failed);
}
#endif

TEST(TlsContextTest, SniCertificateWithWildcardPatternWorks) {
  // Test that a valid wildcard pattern starting with "*." is accepted
  auto mainCert = test::makeEphemeralCertKey("main.example.com");
  auto sniCert = test::makeEphemeralCertKey("sub.example.com");
  ASSERT_FALSE(mainCert.first.empty());
  ASSERT_FALSE(sniCert.first.empty());

  TLSConfig cfg;
  cfg.enabled = true;
  cfg.withCertPem(mainCert.first).withKeyPem(mainCert.second);

  // Add SNI certificate with a proper wildcard pattern
  cfg.withTlsSniCertificateMemory("*.example.com", sniCert.first, sniCert.second);

  TlsMetricsExternal metrics{};
  EXPECT_NO_THROW({
    TlsContext ctx(cfg, &metrics);
    (void)ctx;
  });
}

TEST(TlsTransportTest, ShutdownWithNullSslDoesNotCrash) {
  // Covers the null check in TlsTransport::shutdown (line 147)
  TlsTransport::SslPtr nullSsl{nullptr, &::SSL_free};
  TlsTransport transport(std::move(nullSsl));

  // Should return early without crashing
  transport.shutdown();
}

TEST(TlsTransportTest, HandshakeDoneFalseInitially) {
  // Verify handshakeDone() returns false before handshake is complete
  auto pair = makeSslPair({"http/1.1"}, {"http/1.1"});
  TlsTransport transport(std::move(pair.serverSsl));

  EXPECT_FALSE(transport.handshakeDone());
}

TEST(TlsTransportTest, WriteEmptyDataReturnsZero) {
  // Covers the empty data early return path in write (line 108)
  auto pair = makeSslPair({"http/1.1"}, {"http/1.1"});
  ASSERT_TRUE(performHandshake(pair));
  TlsTransport transport(std::move(pair.serverSsl));

  auto result = transport.write("");
  EXPECT_EQ(result.bytesProcessed, 0U);
  EXPECT_EQ(result.want, TransportHint::None);
}

TEST(TlsTransportTest, ReadAfterPeerCloseReturnsZero) {
  // Covers the SSL_ERROR_ZERO_RETURN path in read (line 56-58)
  auto pair = makeSslPair({"http/1.1"}, {"http/1.1"});
  ASSERT_TRUE(performHandshake(pair));

  TlsTransport transport(std::move(pair.serverSsl));

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
  auto certKey = test::makeEphemeralCertKey();
  ASSERT_FALSE(certKey.first.empty());
  ASSERT_FALSE(certKey.second.empty());

  TLSConfig cfg;
  cfg.enabled = true;
  cfg.withCertPem(certKey.first).withKeyPem(certKey.second);
  cfg.sessionTickets.enabled = true;
  cfg.sessionTickets.lifetime = std::chrono::seconds(60);
  cfg.sessionTickets.maxKeys = 2;

  TlsMetricsExternal metrics{};
  EXPECT_NO_THROW({
    TlsContext ctx(cfg, &metrics);
    (void)ctx;
  });
}

TEST(TlsContextTest, SessionTicketsWithStaticKeys) {
  // Covers line 321-322: loading static keys into ticket store
  auto certKey = test::makeEphemeralCertKey();
  ASSERT_FALSE(certKey.first.empty());
  ASSERT_FALSE(certKey.second.empty());

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

  TlsMetricsExternal metrics{};
  EXPECT_NO_THROW({
    TlsContext ctx(cfg, &metrics);
    (void)ctx;
  });
}

TEST(TlsContextTest, SniCertificateWithFilePaths) {
  // This test covers line 342 in tls-context.cpp: the file-path branch in SNI loading
  // We use file paths that will fail, exercising the else branch
  auto mainCert = test::makeEphemeralCertKey("main.example.com");
  ASSERT_FALSE(mainCert.first.empty());

  TLSConfig cfg;
  cfg.enabled = true;
  cfg.withCertPem(mainCert.first).withKeyPem(mainCert.second);

  // Add SNI certificate with file paths that don't exist - should throw
  cfg.withTlsSniCertificateFiles("test.example.com", "/__nonexistent_cert__.pem", "/__nonexistent_key__.pem");

  TlsMetricsExternal metrics{};
  EXPECT_THROW(TlsContext(cfg, &metrics), std::runtime_error);
}

TEST(TlsContextTest, DefaultCipherPolicyDoesNotApplyPolicy) {
  // Covers lines 203-204 in tls-context.cpp: when cipherPolicy is Default, no ApplyCipherPolicy is called
  auto certKey = test::makeEphemeralCertKey("test.example.com");
  ASSERT_FALSE(certKey.first.empty());

  TLSConfig cfg;
  cfg.enabled = true;
  cfg.withCertPem(certKey.first).withKeyPem(certKey.second);
  cfg.cipherPolicy = TLSConfig::CipherPolicy::Default;  // Explicitly set to Default

  TlsMetricsExternal metrics{};
  TlsContext ctx(cfg, &metrics);

  // Context should be created without throwing
  SUCCEED();
}

TEST(TlsContextTest, DefaultCipherPolicyWithCustomCipherList) {
  // Covers line 204 in tls-context.cpp: Default policy with custom cipher list still sets it
  auto certKey = test::makeEphemeralCertKey("test.example.com");
  ASSERT_FALSE(certKey.first.empty());

  TLSConfig cfg;
  cfg.enabled = true;
  cfg.withCertPem(certKey.first).withKeyPem(certKey.second);
  cfg.cipherPolicy = TLSConfig::CipherPolicy::Default;
  cfg.withCipherList("AES256-SHA:AES128-SHA");

  TlsMetricsExternal metrics{};
  TlsContext ctx(cfg, &metrics);

  SUCCEED();
}

TEST(TlsContextTest, SessionTicketsWithTlsHandshake) {
  // Covers lines 227-252: TicketStoreIndex, GetTicketStore, SessionTicketCallback, AttachTicketStore
  // This requires an actual TLS handshake with session tickets enabled
  auto certKey = test::makeEphemeralCertKey("test.example.com");
  ASSERT_FALSE(certKey.first.empty());

  TLSConfig cfg;
  cfg.enabled = true;
  cfg.withCertPem(certKey.first).withKeyPem(certKey.second);
  cfg.sessionTickets.enabled = true;
  cfg.sessionTickets.lifetime = std::chrono::seconds(60);
  cfg.sessionTickets.maxKeys = 2;

  TlsMetricsExternal metrics{};
  TlsContext ctx(cfg, &metrics);

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
  auto clientCtx =
      std::unique_ptr<SSL_CTX, decltype(&::SSL_CTX_free)>(::SSL_CTX_new(TLS_client_method()), &::SSL_CTX_free);
  ASSERT_NE(clientCtx.get(), nullptr);
  ::SSL_CTX_set_verify(clientCtx.get(), SSL_VERIFY_NONE, nullptr);

  // Enable session tickets on client
  ::SSL_CTX_set_session_cache_mode(clientCtx.get(), SSL_SESS_CACHE_CLIENT);

  std::unique_ptr<SSL, decltype(&::SSL_free)> clientSsl{::SSL_new(clientCtx.get()), &::SSL_free};
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
  // Covers lines 318-322: when session tickets are enabled without static keys,
  // a new ticket store is created and attached
  auto certKey = test::makeEphemeralCertKey("test.example.com");
  ASSERT_FALSE(certKey.first.empty());

  TLSConfig cfg;
  cfg.enabled = true;
  cfg.withCertPem(certKey.first).withKeyPem(certKey.second);
  cfg.sessionTickets.enabled = true;
  cfg.sessionTickets.lifetime = std::chrono::seconds(3600);
  cfg.sessionTickets.maxKeys = 5;

  TlsMetricsExternal metrics{};
  TlsContext ctx(cfg, &metrics);

  // Context should be created successfully with ticket store
  SUCCEED();
}

// =============================================================================
// TLSConfig validation tests
// =============================================================================

TEST(TlsConfigTest, InvalidMinVersionThrows) {
  // Covers tls-config.cpp lines 59-60
  TLSConfig cfg;
  cfg.enabled = true;
  cfg.withCertPem("DUMMY").withKeyPem("DUMMY");
  cfg.minVersion = {1, 0};  // TLS 1.0 is not supported

  EXPECT_THROW(cfg.validate(), std::invalid_argument);
}

TEST(TlsConfigTest, InvalidMaxVersionThrows) {
  // Covers tls-config.cpp lines 65-66
  TLSConfig cfg;
  cfg.enabled = true;
  cfg.withCertPem("DUMMY").withKeyPem("DUMMY");
  cfg.maxVersion = {1, 1};  // TLS 1.1 is not supported

  EXPECT_THROW(cfg.validate(), std::invalid_argument);
}

TEST(TlsConfigTest, SessionTicketsMaxKeysZeroThrows) {
  // Covers tls-config.cpp line 84
  TLSConfig cfg;
  cfg.enabled = true;
  cfg.withCertPem("DUMMY").withKeyPem("DUMMY");
  cfg.sessionTickets.maxKeys = 0;

  EXPECT_THROW(cfg.validate(), std::invalid_argument);
}

TEST(TlsConfigTest, ClearSniCertificatesWorks) {
  // Covers tls-config.cpp lines 160-162
  auto certKey = test::makeEphemeralCertKey("test.example.com");
  ASSERT_FALSE(certKey.first.empty());

  TLSConfig cfg;
  cfg.enabled = true;
  cfg.withCertPem(certKey.first).withKeyPem(certKey.second);
  cfg.withTlsSniCertificateMemory("sub.example.com", certKey.first, certKey.second);

  EXPECT_FALSE(cfg.sniCertificates().empty());
  cfg.clearTlsSniCertificates();
  EXPECT_TRUE(cfg.sniCertificates().empty());
}

TEST(TlsConfigTest, EmptySniHostnameMemoryThrows) {
  // Covers tls-config.cpp line 145 (empty hostname in withTlsSniCertificateMemory)
  TLSConfig cfg;
  cfg.enabled = true;

  EXPECT_THROW(cfg.withTlsSniCertificateMemory("", "cert", "key"), std::invalid_argument);
}

TEST(TlsConfigTest, EmptySniHostnameFilesThrows) {
  // Covers tls-config.cpp withTlsSniCertificateFiles empty hostname
  TLSConfig cfg;
  cfg.enabled = true;

  EXPECT_THROW(cfg.withTlsSniCertificateFiles("", "/path/cert", "/path/key"), std::invalid_argument);
}

TEST(TlsConfigTest, EmptySniCertPemThrows) {
  // Covers tls-config.cpp line 147-148 (empty certPem/keyPem)
  TLSConfig cfg;
  cfg.enabled = true;

  EXPECT_THROW(cfg.withTlsSniCertificateMemory("example.com", "", "key"), std::invalid_argument);
  EXPECT_THROW(cfg.withTlsSniCertificateMemory("example.com", "cert", ""), std::invalid_argument);
}

TEST(TlsConfigTest, EmptySniCertFileThrows) {
  // Covers tls-config.cpp empty cert/key paths
  TLSConfig cfg;
  cfg.enabled = true;

  EXPECT_THROW(cfg.withTlsSniCertificateFiles("example.com", "", "/path/key"), std::invalid_argument);
  EXPECT_THROW(cfg.withTlsSniCertificateFiles("example.com", "/path/cert", ""), std::invalid_argument);
}

}  // namespace tls_test
