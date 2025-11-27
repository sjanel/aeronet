#include <fcntl.h>
#include <gtest/gtest.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/sslerr.h>
#include <openssl/types.h>
#include <sys/socket.h>

#include <cerrno>
#include <chrono>
#include <initializer_list>
#include <memory>
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

namespace aeronet::tls_test {
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
  auto cert = aeronet::test::makeEphemeralCertKey();
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
  auto info = collectTlsHandshakeInfo(pair.serverSsl.get(), start);
  EXPECT_EQ(info.selectedAlpn, "http/1.1");
  EXPECT_FALSE(info.negotiatedCipher.empty());
  EXPECT_FALSE(info.negotiatedVersion.empty());
  EXPECT_GT(info.durationNs, 0U);

  TlsMetricsInternal metrics;
  auto tlsInfo = finalizeTlsHandshake(pair.serverSsl.get(), pair.serverFd.fd(), true, start, metrics);
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
  auto certKey = aeronet::test::makeEphemeralCertKey();
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
  auto certKey = aeronet::test::makeEphemeralCertKey();
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
  auto certKey = aeronet::test::makeEphemeralCertKey();
  ASSERT_FALSE(certKey.first.empty());
  ASSERT_FALSE(certKey.second.empty());
  TLSConfig cfg;
  cfg.enabled = true;
  cfg.withCertPem(certKey.first).withKeyPem(certKey.second);
  cfg.withTlsMaxVersion("TLS1.1");
  TlsMetricsExternal metrics{};
  EXPECT_THROW(TlsContext(cfg, &metrics), std::runtime_error);
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
  auto certA = aeronet::test::makeEphemeralCertKey("server-a");
  auto certB = aeronet::test::makeEphemeralCertKey("server-b");
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
  auto certKey = aeronet::test::makeEphemeralCertKey();
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
  auto certKey = aeronet::test::makeEphemeralCertKey();
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

TEST(TlsTransportTest, HandshakeSyscallReadTreatedAsRetry) {
  auto pair = makeSslPair({"http/1.1"}, {"http/1.1"});
  ControlledBioState readState{};
  ControlledBioState writeState{};
  attachControlledBios(pair.serverSsl.get(), readState, writeState);
  pair.serverFd.close();
  TlsTransport transport(std::move(pair.serverSsl));
  char tmp[1]{};
  ERR_clear_error();
  errno = EAGAIN;
  auto res = transport.read(tmp, sizeof(tmp));
  EXPECT_EQ(res.want, TransportHint::ReadReady);
  EXPECT_EQ(res.bytesProcessed, 0U);
}

TEST(TlsTransportTest, HandshakeSyscallReadFatalSetsError) {
  auto pair = makeSslPair({"http/1.1"}, {"http/1.1"});
  ControlledBioState readState{};
  readState.errnoValue = EBADF;
  ControlledBioState writeState{};
  writeState.errnoValue = EBADF;
  attachControlledBios(pair.serverSsl.get(), readState, writeState);
  pair.serverFd.close();
  TlsTransport transport(std::move(pair.serverSsl));
  char tmp[1]{};
  ERR_clear_error();
  errno = EBADF;
  auto res = transport.read(tmp, sizeof(tmp));
  EXPECT_EQ(res.want, TransportHint::Error);
  EXPECT_EQ(res.bytesProcessed, 0U);
}

TEST(TlsTransportTest, HandshakeSyscallWriteTreatedAsRetry) {
  auto pair = makeSslPair({"http/1.1"}, {"http/1.1"});
  ControlledBioState readState{};
  ControlledBioState writeState{};
  attachControlledBios(pair.serverSsl.get(), readState, writeState);
  pair.serverFd.close();
  TlsTransport transport(std::move(pair.serverSsl));
  ERR_clear_error();
  errno = EAGAIN;
  auto res = transport.write("X");
  EXPECT_EQ(res.want, TransportHint::WriteReady);
  EXPECT_EQ(res.bytesProcessed, 0U);
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

}  // namespace aeronet::tls_test
