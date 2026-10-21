#include <gtest/gtest.h>
#include <openssl/asn1.h>
#include <openssl/bio.h>
#include <openssl/ec.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/objects.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/types.h>
#include <openssl/x509.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include "aeronet/tls-raii.hpp"

#ifdef AERONET_LINUX
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include "aeronet/base-fd.hpp"
#include "aeronet/http-request-view.hpp"
#include "aeronet/http-response-writer.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/router-config.hpp"
#include "aeronet/server-stats.hpp"
#include "aeronet/single-http-server.hpp"
#include "aeronet/system-error.hpp"
#include "aeronet/temp-file.hpp"
#include "aeronet/test-tls-helper.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_server_tls_fixture.hpp"
#include "aeronet/test_tls_client.hpp"
#include "aeronet/test_util.hpp"
#include "aeronet/tls-handshake-callback.hpp"
#include "aeronet/tls-handshake-failure-reasons.hpp"

using namespace aeronet;
using namespace std::chrono_literals;

namespace {
// Certificate cache to avoid expensive RSA keygen + signing in every test.
struct CertKeyCache {
  std::pair<std::string, std::string> localhost;
  std::pair<std::string, std::string> server;
  std::pair<std::string, std::string> client;

  static CertKeyCache& Get() {
    static CertKeyCache instance{};
    return instance;
  }

 private:
  CertKeyCache() {
    // Pre-generate all certificate pairs on first access
    localhost = test::MakeEphemeralCertKey("localhost");
    server = test::MakeEphemeralCertKey("server");
    client = test::MakeEphemeralCertKey("client");
  }
};

// Upper bound to observe the server closing a rejected connection. WaitForPeerClose returns as soon as the peer closes,
// so a generous bound costs nothing: it only absorbs event loop stalls of a loaded machine (sanitizers, parallel
// ctest), which reached several hundreds of ms.
constexpr auto kRejectedCloseTimeout = 5s;

// The server counts a strict ALPN mismatch once the handshake failed on its side, possibly after the client saw it
// fail: waits for its statistics to report 'expected' of them.
ServerStats WaitForStrictAlpnMismatches(const auto& server, uint64_t expected) {
  const auto deadline = std::chrono::steady_clock::now() + kRejectedCloseTimeout;
  ServerStats stats = server.stats();
  while (stats.tlsAlpnStrictMismatches < expected && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(1ms);
    stats = server.stats();
  }
  return stats;
}
}  // namespace

#ifdef AERONET_ENABLE_HTTP2
TEST(HttpTlsAlpnMismatch, HandshakeFailsWhenNoCommonProtocolAndMustMatch) {
  test::TlsTestServer ts({"http/1.1", "h2"}, [](HttpServerConfig& cfg) {
    cfg.withTlsAlpnMustMatch(true);
    cfg.withTlsHandshakeLogging(true);
  });
  auto port = ts.port();
  ts.setDefault(
      [](const HttpRequestView& req) { return HttpResponse(std::string("ALPN:") + std::string(req.alpnProtocol())); });
  // Offer only a mismatching ALPN; since TlsClient uses options, construct with protoX.
  test::TlsClient::Options opts;
  opts.alpn = {"protoX"};
  test::TlsClient client(port, opts);

  ASSERT_FALSE(client.handshakeOk());
  EXPECT_EQ(WaitForStrictAlpnMismatches(ts, 1).tlsAlpnStrictMismatches, 1U);
}

TEST(HttpTlsAlpnNonStrict, MismatchAllowedAndNoMetricIncrement) {
  std::string capturedAlpn;
  ServerStats statsAfter{};
  {
    // Server prefers h2, but does NOT enforce match.
    test::TlsTestServer ts({"h2"});
    auto port = ts.port();
    ts.setDefault([&](const HttpRequestView& req) {
      if (!req.alpnProtocol().empty()) {
        capturedAlpn = std::string(req.alpnProtocol());
      } else {
        capturedAlpn.clear();
      }
      return HttpResponse("NS");
    });
    test::TlsClient::Options opts;
    opts.alpn = {"foo"};  // no overlap
    test::TlsClient client(port, opts);
    ASSERT_TRUE(client.handshakeOk());
    auto resp = client.get("/non_strict");
    statsAfter = ts.stats();
    ts.stop();
    ASSERT_TRUE(resp.starts_with("HTTP/1.1 200"));
  }
  // ALPN not negotiated => empty string
  ASSERT_TRUE(capturedAlpn.empty());
  ASSERT_EQ(statsAfter.tlsAlpnStrictMismatches, 0U);
  // Distribution should NOT contain client-only protocol
  for (const auto& kv : statsAfter.tlsAlpnDistribution) {
    ASSERT_NE(kv.first, std::string("foo"));
  }
}
#endif

TEST(HttpTlsAlpnMismatch, StrictMismatchMetricSurvivesServerMove) {
  auto defaultPair = CertKeyCache::Get().localhost;

  HttpServerConfig cfg;
  cfg.withTlsCertKeyMemory(defaultPair.first, defaultPair.second);
  cfg.withTlsAlpnProtocols({"http/1.1"});
  cfg.withTlsAlpnMustMatch(true);

  SingleHttpServer server(cfg, RouterConfig{});
  server.router().setDefault([](const HttpRequestView&) { return HttpResponse("MOVE"); });

  // Move while idle: TLS context and its ALPN callback data are reused.
  SingleHttpServer moved(std::move(server));

  auto handle = moved.startDetached();
  ASSERT_TRUE(handle.started());
  ASSERT_TRUE(test::WaitForServer(moved, true));

  test::TlsClient::Options opts;
  opts.alpn = {"protoX"};
  test::TlsClient client(moved.port(), opts);
  ASSERT_FALSE(client.handshakeOk());

  EXPECT_EQ(WaitForStrictAlpnMismatches(moved, 1).tlsAlpnStrictMismatches, 1U);

  handle.stop();
}

TEST(HttpTlsAlpnMismatch, StrictMismatchCountSurvivesTlsContextRebuild) {
  test::TlsTestServer ts({"http/1.1"}, [](HttpServerConfig& cfg) { cfg.withTlsAlpnMustMatch(true); });
  test::TlsClient::Options opts;
  opts.alpn = {"protoX"};
  {
    test::TlsClient client(ts.port(), opts);
    ASSERT_FALSE(client.handshakeOk());
  }
  ASSERT_EQ(WaitForStrictAlpnMismatches(ts, 1).tlsAlpnStrictMismatches, 1U);

  // Any TLS change rebuilds the TLS context of the server, as does a certificate rotation.
  ts.server.postConfigUpdate([](HttpServerConfig& cfg) { cfg.withTlsHandshakeLogging(true); });
  {
    test::TlsClient client(ts.port(), opts);
    ASSERT_FALSE(client.handshakeOk());
  }
  EXPECT_EQ(WaitForStrictAlpnMismatches(ts, 2).tlsAlpnStrictMismatches, 2U);
}

extern std::atomic<int> g_aeronetTestFailNextSslNew;
extern std::atomic<int> g_aeronetTestFailNextSslSetFd;
extern std::atomic<int> g_aeronetTestFailNextSslSetAcceptState;

namespace {

// Makes a step of the TLS setup of the next accepted connection fail through failNext, and checks that the failure is
// reported with the given reason, to the handshake callback and in the stats.
void ExpectTlsSetupFailureReported(std::atomic<int>& failNext, std::string_view reason) {
  test::TlsTestServer ts;

  std::atomic_bool callbackOK{false};

  ts.server.server.setTlsHandshakeCallback([&](const TlsHandshakeEvent& ev) {
    if (ev.result == TlsHandshakeEvent::Result::Failed && ev.reason == reason) {
      callbackOK.store(true);
    }
  });

  ts.setDefault([](const HttpRequestView&) { return HttpResponse("OK"); });

  failNext.store(1, std::memory_order_relaxed);
  {
    test::ClientConnection cnx(ts.port());
    // failure triggered; callback will be observed via active wait
  }

  // Active wait for callback to be invoked (deadline-style)
  const auto deadline = std::chrono::steady_clock::now() + 1s;
  while (std::chrono::steady_clock::now() < deadline && !callbackOK.load()) {
    std::this_thread::sleep_for(1ms);
  }
  const auto st = ts.stats();
  ts.stop();

  EXPECT_TRUE(callbackOK.load());

  bool found = false;
  for (const auto& kv : st.tlsHandshakeFailureReasons) {
    if (kv.first == reason) {
      found = true;
      EXPECT_GE(kv.second, 1UL);
    }
  }
  EXPECT_TRUE(found);
}

}  // namespace

TEST(HttpTlsHandshakeCallback, ExceptionRaisedInCallbackIsLoggedAndIgnored) {
  test::TlsTestServer ts({"http/1.1"}, [](HttpServerConfig& cfg) {
    cfg.withTlsAlpnMustMatch(true);
    cfg.withTlsHandshakeLogging(true);
  });

  ts.server.server.setTlsHandshakeCallback([&]([[maybe_unused]] const TlsHandshakeEvent& ev) {
    throw std::runtime_error("Simulated exception in handshake callback");
  });

  ts.setDefault([](const HttpRequestView&) { return HttpResponse("OK"); });

  test::TlsClient::Options opt;
  opt.alpn = {"http/1.1"};
  test::TlsClient client(ts.port(), std::move(opt));
  ASSERT_TRUE(client.handshakeOk());
  auto resp = client.get("/", {});
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 200"));
}

TEST(HttpTlsHandshakeCallback, UnknownExceptionRaisedInCallbackIsLoggedAndIgnored) {
  test::TlsTestServer ts({"http/1.1"}, [](HttpServerConfig& cfg) {
    cfg.withTlsAlpnMustMatch(true);
    cfg.withTlsHandshakeLogging(true);
  });

  // NOLINTNEXTLINE(bugprone-std-exception-baseclass)
  ts.server.server.setTlsHandshakeCallback([&]([[maybe_unused]] const TlsHandshakeEvent& ev) { throw 42; });

  ts.setDefault([](const HttpRequestView&) { return HttpResponse("OK"); });

  test::TlsClient::Options opt;
  opt.alpn = {"http/1.1"};
  test::TlsClient client(ts.port(), std::move(opt));
  ASSERT_TRUE(client.handshakeOk());
  auto resp = client.get("/", {});
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 200"));
}

TEST(HttpTlsHandshakeCallback, EmitsSuccessEventWithNegotiatedAlpn) {
  test::TlsTestServer ts({"http/1.1"}, [](HttpServerConfig& cfg) {
    cfg.withTlsAlpnMustMatch(true);
    cfg.withTlsHandshakeLogging(true);
  });

  std::atomic<uint64_t> successCount{0};
  std::mutex mu;
  std::string lastAlpn;

  ts.server.server.setTlsHandshakeCallback([&](const TlsHandshakeEvent& ev) {
    if (ev.result == TlsHandshakeEvent::Result::Succeeded) {
      ++successCount;
      std::scoped_lock lock(mu);
      lastAlpn = std::string(ev.selectedAlpn);
    }
  });

  ts.setDefault([](const HttpRequestView&) { return HttpResponse("OK"); });

  test::TlsClient::Options opt;
  opt.alpn = {"http/1.1"};
  test::TlsClient client(ts.port(), std::move(opt));
  ASSERT_TRUE(client.handshakeOk());
  (void)client.get("/", {});

  EXPECT_GE(successCount.load(), 1UL);
  {
    std::scoped_lock lock(mu);
    EXPECT_EQ(lastAlpn, "http/1.1");
  }
}

#ifdef AERONET_ENABLE_HTTP2
TEST(HttpTlsHandshakeCallback, EmitsFailureEventAndBucketsReasonOnStrictAlpnMismatch) {
  test::TlsTestServer ts({"h2"}, [](HttpServerConfig& cfg) {
    cfg.withTlsAlpnMustMatch(true);
    cfg.withTlsHandshakeLogging(true);
  });

  std::atomic_bool callbackOK{false};

  ts.server.server.setTlsHandshakeCallback([&](const TlsHandshakeEvent& ev) {
    if (ev.result == TlsHandshakeEvent::Result::Failed && ev.reason == kTlsHandshakeFailureReasonAlpnStrictMismatch) {
      callbackOK.store(true);
    }
  });

  ts.setDefault([](const HttpRequestView&) { return HttpResponse("OK"); });

  {
    test::TlsClient::Options opt;
    opt.alpn = {"http/1.1"};
    test::TlsClient client(ts.port(), std::move(opt));
    // Client handshake should fail due to ALPN mismatch
    EXPECT_FALSE(client.handshakeOk());
  }  // client goes out of scope, connection closed

  // Poll for both callback and stats to be populated (server needs time to process connection close)
  ServerStats st{};
  bool found = false;
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (std::chrono::steady_clock::now() < deadline && (!callbackOK.load() || !found)) {
    std::this_thread::sleep_for(ts.server.server.config().pollInterval + 100us);
    st = ts.stats();
    for (const auto& kv : st.tlsHandshakeFailureReasons) {
      if (kv.first == kTlsHandshakeFailureReasonAlpnStrictMismatch && kv.second >= 1) {
        found = true;
        break;
      }
    }
  }

  EXPECT_TRUE(callbackOK.load());
  EXPECT_TRUE(found);
}
#endif

TEST(HttpTlsHandshakeCallback, EmitsRejectedEventAndBucketsReasonOnConcurrencyLimit) {
  test::TlsTestServer ts({}, [](HttpServerConfig& cfg) {
    cfg.tls.maxConcurrentHandshakes = 1;
    cfg.tls.handshakeTimeout = 5s;
  });

  std::atomic_bool callbackOK{false};
  std::string_view lastReason;

  ts.server.server.setTlsHandshakeCallback([&](const TlsHandshakeEvent& ev) {
    if (ev.result == TlsHandshakeEvent::Result::Rejected) {
      lastReason = ev.reason;
      callbackOK.store(true);
    }
  });

  ts.setDefault([](const HttpRequestView&) { return HttpResponse("OK"); });

  // Hold one handshake slot open by connecting and not sending a TLS ClientHello.
  test::ClientConnection hold(ts.port());

  // Second connection should be rejected immediately.
  test::ClientConnection rejected(ts.port());
  EXPECT_TRUE(test::WaitForPeerClose(rejected.fd(), kRejectedCloseTimeout));

  // active wait for callback to be invoked
  const auto now = std::chrono::steady_clock::now();
  const auto deadline = now + 1s;
  while (std::chrono::steady_clock::now() < deadline && !callbackOK.load()) {
    std::this_thread::sleep_for(1ms);
  }
  const auto st = ts.stats();

  EXPECT_TRUE(callbackOK.load());
  EXPECT_EQ(lastReason, kTlsHandshakeFailureReasonRejectedConcurrency);
  EXPECT_GE(st.tlsHandshakesRejectedConcurrency, 1UL);

  bool found = false;
  for (const auto& kv : st.tlsHandshakeFailureReasons) {
    if (kv.first == kTlsHandshakeFailureReasonRejectedConcurrency) {
      found = true;
      EXPECT_GE(kv.second, 1UL);
    }
  }
  EXPECT_TRUE(found);
}

TEST(HttpTlsHandshakeCallback, EmitsRejectedEventAndBucketsReasonOnRateLimit) {
  test::TlsTestServer ts({}, [](HttpServerConfig& cfg) {
    cfg.tls.maxConcurrentHandshakes = 0;  // avoid interfering with this test
    cfg.tls.handshakeRateLimitPerSecond = 1;
    cfg.tls.handshakeRateLimitBurst = 1;
    cfg.tls.handshakeTimeout = 5s;
  });

  std::atomic_bool callbackOK{false};
  std::string_view lastReason;

  ts.server.server.setTlsHandshakeCallback([&](const TlsHandshakeEvent& ev) {
    if (ev.result == TlsHandshakeEvent::Result::Rejected) {
      lastReason = ev.reason;
      callbackOK.store(true);
    }
  });

  ts.setDefault([](const HttpRequestView&) { return HttpResponse("OK"); });

  // The readiness probe of the test server (a plain TCP connection) already consumes the single token: this first
  // connection only guarantees that the bucket is empty, whatever the server startup does.
  test::ClientConnection first(ts.port());

  // Second connection in the same second should be rejected.
  test::ClientConnection rejected(ts.port());
  EXPECT_TRUE(test::WaitForPeerClose(rejected.fd(), kRejectedCloseTimeout));

  const auto now = std::chrono::steady_clock::now();
  const auto deadline = now + 1s;
  while (std::chrono::steady_clock::now() < deadline && !callbackOK.load()) {
    std::this_thread::sleep_for(1ms);
  }
  const auto st = ts.stats();

  EXPECT_EQ(lastReason, kTlsHandshakeFailureReasonRejectedRateLimit);
  EXPECT_GE(st.tlsHandshakesRejectedRateLimit, 1UL);

  bool found = false;
  for (const auto& kv : st.tlsHandshakeFailureReasons) {
    if (kv.first == kTlsHandshakeFailureReasonRejectedRateLimit) {
      found = true;
      EXPECT_GE(kv.second, 1UL);
    }
  }
  EXPECT_TRUE(found);
}

TEST(HttpTlsHandshakeCallback, EmitsFailureEventAndBucketsReasonOnHandshakeTimeout) {
  test::TlsTestServer ts({}, [](HttpServerConfig& cfg) { cfg.tls.handshakeTimeout = 50ms; });

  std::atomic_bool callbackOK{false};
  std::string_view lastReason;

  ts.server.server.setTlsHandshakeCallback([&](const TlsHandshakeEvent& ev) {
    if (ev.result == TlsHandshakeEvent::Result::Failed) {
      lastReason = ev.reason;
      callbackOK.store(true);
    }
  });

  ts.setDefault([](const HttpRequestView&) { return HttpResponse("OK"); });

  test::ClientConnection stalled(ts.port());
  EXPECT_TRUE(test::WaitForPeerClose(stalled.fd(), 1500ms));

  // Active wait for callback to be invoked (deadline-style)
  const auto now = std::chrono::steady_clock::now();
  const auto deadline = now + 1s;
  while (std::chrono::steady_clock::now() < deadline && !callbackOK.load()) {
    std::this_thread::sleep_for(1ms);
  }
  const auto st = ts.stats();
  ts.stop();

  EXPECT_TRUE(callbackOK.load());
  EXPECT_EQ(lastReason, kTlsHandshakeFailureReasonHandshakeTimeout);
  EXPECT_GE(st.tlsHandshakesFailed, 1UL);

  bool found = false;
  for (const auto& kv : st.tlsHandshakeFailureReasons) {
    if (kv.first == kTlsHandshakeFailureReasonHandshakeTimeout) {
      found = true;
      EXPECT_GE(kv.second, 1UL);
    }
  }
  EXPECT_TRUE(found);
}

TEST(HttpTlsHandshakeCallback, BucketsReasonWhenSslNewFails) {
  ExpectTlsSetupFailureReported(g_aeronetTestFailNextSslNew, kTlsHandshakeFailureReasonSslNewFailed);
}

TEST(HttpTlsHandshakeCallback, RefillsRateLimitAfterInterval) {
  test::TlsTestServer ts({}, [](HttpServerConfig& cfg) {
    cfg.tls.maxConcurrentHandshakes = 0;  // avoid concurrency interference
    cfg.tls.handshakeRateLimitPerSecond = 1;
    cfg.tls.handshakeRateLimitBurst = 1;
    cfg.tls.handshakeTimeout = 500ms;
  });

  ts.setDefault([](const HttpRequestView&) { return HttpResponse("OK"); });

  // Consume the single token (already taken by the readiness probe of the test server, as above)
  test::ClientConnection first(ts.port());
  const auto firstHandshakeFinished = std::chrono::steady_clock::now();
  // Wait well over one second to ensure the seconds-based refill calculation
  // yields addIntervals > 0 in connection-manager.cpp (which uses whole-second intervals).
  // 1500 ms gives comfortable margin for Windows timer resolution (~15.6 ms) and
  // the scheduling delta between client-side sleep and server-side accept timestamps.
  const auto waitUntil = firstHandshakeFinished + 1500ms;

  // Second connection in same second should be rejected
  test::ClientConnection rejected(ts.port());
  EXPECT_TRUE(test::WaitForPeerClose(rejected.fd(), kRejectedCloseTimeout));

  // Wait more than one second so refill happens (addIntervals > 0)
  const auto now = std::chrono::steady_clock::now();
  if (now < waitUntil) {
    std::this_thread::sleep_for(waitUntil - now);
  }

  // Now a new connection should be accepted because tokens were refilled
  test::ClientConnection after(ts.port());
  // Use a timeout well below handshakeTimeout (500 ms) to avoid a race:
  // ClientConnection is plain TCP so the server-side TLS handshake never
  // completes; it will be closed when handshakeTimeout fires.  We only need
  // to verify that the connection is *not* closed immediately (= accepted).
  EXPECT_FALSE(test::WaitForPeerClose(after.fd(), 250ms));
  ts.stop();
}

TEST(HttpTlsHandshakeCallback, BucketsReasonWhenSslSetFdFails) {
  ExpectTlsSetupFailureReported(g_aeronetTestFailNextSslSetFd, kTlsHandshakeFailureReasonSslSetFdFailed);
}

// Since OpenSSL 3.2, SSL_set_accept_state() fails on an allocation failure (silently: the connection is then in error).
TEST(HttpTlsHandshakeCallback, BucketsReasonWhenSslSetAcceptStateFails) {
  ExpectTlsSetupFailureReported(g_aeronetTestFailNextSslSetAcceptState,
                                kTlsHandshakeFailureReasonSslSetAcceptStateFailed);
}

TEST(HttpTlsHandshakeTest, EcdsaCertificateHandshakeWorks) {
  auto [certPem, keyPem] = test::MakeEphemeralCertKey("ecdsa", 3600, test::KeyAlgorithm::EcdsaP256);
  HttpServerConfig cfg;
  cfg.withTlsCertKeyMemory(certPem, keyPem);

  test::TestServer ts(std::move(cfg));
  test::TlsClient client(ts.port());
  EXPECT_TRUE(client.handshakeOk());
  EXPECT_EQ(client.peerCommonName(), "ecdsa");
}

TEST(HttpTlsHandshakeTest, HotCertReloadSwapsCertificateForNewConnections) {
  auto [certPem1, keyPem1] = CertKeyCache::Get().server;
  auto [certPem2, keyPem2] = CertKeyCache::Get().client;

  HttpServerConfig cfg;
  cfg.withTlsCertKeyMemory(certPem1, keyPem1);

  test::TestServer ts(std::move(cfg));

  {
    test::TlsClient before(ts.port());
    EXPECT_TRUE(before.handshakeOk());
    EXPECT_EQ(before.peerCommonName(), "server");
  }

  TLSConfig tls;
  tls.enabled = true;
  tls.withCertPem(certPem2).withKeyPem(keyPem2);
  ts.postConfigUpdate([tls = std::move(tls)](HttpServerConfig& cfg) mutable { cfg.tls = std::move(tls); });

  {
    test::TlsClient after(ts.port());
    EXPECT_TRUE(after.handshakeOk());
    EXPECT_EQ(after.peerCommonName(), "client");
  }
}

namespace {

// Common name of the certificate presented to a new connection, empty if the handshake fails.
std::string NewConnectionCommonName(uint16_t port) {
  test::TlsClient client(port);
  return client.handshakeOk() ? client.peerCommonName() : std::string{};
}

// Session of a new connection, after a request so that the client processed the TLS 1.3 NewSessionTicket messages.
// Returns a copy: OpenSSL marks the session of a connection freed without a TLS shutdown as not resumable.
test::TlsClient::SessionUniquePtr NewSession(uint16_t port) {
  test::TlsClient client(port);
  if (!client.handshakeOk() || !client.get("/", {}).starts_with("HTTP/1.1 200")) {
    return {nullptr, ::SSL_SESSION_free};
  }
  return {::SSL_SESSION_dup(client.get1Session().get()), ::SSL_SESSION_free};
}

// Whether a new connection offering the given session is resumed by the server.
bool NewConnectionResumes(uint16_t port, SSL_SESSION* session) {
  test::TlsClient::Options opts;
  opts.reuseSession = session;
  test::TlsClient client(port, std::move(opts));
  return client.handshakeOk() && ::SSL_session_reused(client.sslHandle()) == 1;
}

// Certificate and key files rotated at the same paths, as certbot or cert-manager do.
struct RotatedTlsFiles {
  explicit RotatedTlsFiles(const std::pair<std::string, std::string>& certKey) { rewriteInPlace(certKey); }

  void rewriteInPlace(const std::pair<std::string, std::string>& certKey) const {
    test::WriteFile(certPath, certKey.first);
    test::WriteFile(keyPath, certKey.second);
  }

  void replaceAtomically(const std::pair<std::string, std::string>& certKey) const {
    test::ReplaceFileAtomically(certPath, certKey.first);
    test::ReplaceFileAtomically(keyPath, certKey.second);
  }

  [[nodiscard]] HttpServerConfig serverConfig() const {
    HttpServerConfig cfg;
    cfg.withTlsCertKey(certPath.string(), keyPath.string());
    return cfg;
  }

  test::ScopedTempDir dir;
  std::filesystem::path certPath{dir.dirPath() / "fullchain.pem"};
  std::filesystem::path keyPath{dir.dirPath() / "privkey.pem"};
};

}  // namespace

TEST(HttpTlsHotReload, FilesRewrittenInPlaceAreReloadedByAnEmptyUpdate) {
  // "localhost" has a longer common name than "server": the rewritten certificate file changes size.
  RotatedTlsFiles files(CertKeyCache::Get().server);
  test::TestServer ts(files.serverConfig());
  ts.router().setDefault([](const HttpRequestView&) { return HttpResponse("OK"); });

  test::TlsClient existing(ts.port());
  ASSERT_TRUE(existing.handshakeOk());
  EXPECT_EQ(existing.peerCommonName(), "server");

  files.rewriteInPlace(CertKeyCache::Get().localhost);
  // Files are only checked when a config update is applied.
  EXPECT_EQ(NewConnectionCommonName(ts.port()), "server");

  ts.postConfigUpdate([](HttpServerConfig&) {});
  EXPECT_EQ(NewConnectionCommonName(ts.port()), "localhost");
  EXPECT_EQ(ts.server.config().tls.certFile(), files.certPath.string());

  // Connections established before the reload keep their TLS session.
  EXPECT_TRUE(existing.get("/existing", {}).starts_with("HTTP/1.1 200"));
}

TEST(HttpTlsHotReload, AtomicallyReplacedFilesAreReloadedByAnUnrelatedUpdate) {
  RotatedTlsFiles files(CertKeyCache::Get().server);
  test::TestServer ts(files.serverConfig());
  EXPECT_EQ(NewConnectionCommonName(ts.port()), "server");

  files.replaceAtomically(CertKeyCache::Get().client);
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.withMaxBodyBytes(cfg.maxBodyBytes + 1); });
  EXPECT_EQ(NewConnectionCommonName(ts.port()), "client");
}

#ifdef AERONET_POSIX
TEST(HttpTlsHotReload, KubernetesSecretVolumeUpdateIsReloaded) {
  // Layout of a Kubernetes secret volume: tls.crt -> ..data/tls.crt, ..data -> ..<timestamp>, the kubelet publishing
  // a new version by swapping the ..data link.
  test::ScopedTempDir dir;
  const auto& root = dir.dirPath();
  const auto writeVersion = [&root](const char* version, const std::pair<std::string, std::string>& certKey) {
    std::filesystem::create_directory(root / version);
    test::WriteFile(root / version / "tls.crt", certKey.first);
    test::WriteFile(root / version / "tls.key", certKey.second);
  };
  writeVersion("..2026_v1", CertKeyCache::Get().server);
  writeVersion("..2026_v2", CertKeyCache::Get().client);
  test::ReplaceSymlinkAtomically(root / "..data", "..2026_v1");
  std::filesystem::create_symlink("..data/tls.crt", root / "tls.crt");
  std::filesystem::create_symlink("..data/tls.key", root / "tls.key");

  HttpServerConfig cfg;
  cfg.withTlsCertKey((root / "tls.crt").string(), (root / "tls.key").string());
  test::TestServer ts(std::move(cfg));
  EXPECT_EQ(NewConnectionCommonName(ts.port()), "server");

  test::ReplaceSymlinkAtomically(root / "..data", "..2026_v2");
  ts.postConfigUpdate([](HttpServerConfig&) {});
  EXPECT_EQ(NewConnectionCommonName(ts.port()), "client");
}
#endif

TEST(HttpTlsHotReload, InvalidCertificateUpdateKeepsPreviousCertificateAndConfig) {
  RotatedTlsFiles files(CertKeyCache::Get().server);
  test::ScopedTempFile invalidCertFile(files.dir, "not a certificate");
  test::TestServer ts(files.serverConfig());
  const auto previousMaxBodyBytes = ts.server.config().maxBodyBytes;

  // New path to an invalid certificate: the whole update is rejected.
  ts.postConfigUpdate([&invalidCertFile](HttpServerConfig& cfg) {
    cfg.withMaxBodyBytes(cfg.maxBodyBytes + 1);
    cfg.tls.withCertFile(invalidCertFile.filePath().string());
  });
  EXPECT_TRUE(ts.server.isRunning());
  EXPECT_EQ(ts.server.config().tls.certFile(), files.certPath.string());
  EXPECT_EQ(ts.server.config().maxBodyBytes, previousMaxBodyBytes);
  EXPECT_EQ(NewConnectionCommonName(ts.port()), "server");

  // Certificate file rewritten in place with an invalid content: the previous TLS context is kept.
  test::WriteFile(files.certPath, "not a certificate either");
  ts.postConfigUpdate([](HttpServerConfig&) {});
  EXPECT_TRUE(ts.server.isRunning());
  EXPECT_EQ(NewConnectionCommonName(ts.port()), "server");

  // Certificate not matching the private key: rejected as well.
  test::WriteFile(files.certPath, CertKeyCache::Get().localhost.first);
  ts.postConfigUpdate([](HttpServerConfig&) {});
  EXPECT_EQ(NewConnectionCommonName(ts.port()), "server");

  // Once the files are fixed, the next update reloads them.
  files.rewriteInPlace(CertKeyCache::Get().localhost);
  ts.postConfigUpdate([](HttpServerConfig&) {});
  EXPECT_EQ(NewConnectionCommonName(ts.port()), "localhost");
}

TEST(HttpTlsHotReload, SessionTicketsIssuedBeforeAReloadStillResume) {
  RotatedTlsFiles files(CertKeyCache::Get().server);
  HttpServerConfig cfg = files.serverConfig();
  cfg.tls.withTlsSessionTickets(true);
  test::TestServer ts(std::move(cfg));
  ts.router().setDefault([](const HttpRequestView&) { return HttpResponse("OK"); });

  // A TLS 1.3 session is resumed at most once: take a new one before each check.
  auto session = NewSession(ts.port());
  ASSERT_NE(session.get(), nullptr);
  EXPECT_TRUE(NewConnectionResumes(ts.port(), session.get()));

  // Certificate rotated in place: the new context keeps the ticket keys of the previous one.
  session = NewSession(ts.port());
  files.rewriteInPlace(CertKeyCache::Get().localhost);
  ts.postConfigUpdate([](HttpServerConfig&) {});
  EXPECT_EQ(NewConnectionCommonName(ts.port()), "localhost");
  EXPECT_TRUE(NewConnectionResumes(ts.port(), session.get()));

  // Another TLS setting changed: the ticket keys are kept too.
  session = NewSession(ts.port());
  ts.postConfigUpdate([](HttpServerConfig& config) { config.withTlsHandshakeTimeout(5s); });
  EXPECT_TRUE(NewConnectionResumes(ts.port(), session.get()));

  // Session ticket settings changed: new keys, the tickets issued before are no longer accepted.
  session = NewSession(ts.port());
  ts.postConfigUpdate([](HttpServerConfig& config) { config.tls.withTlsSessionTicketMaxKeys(3); });
  EXPECT_FALSE(NewConnectionResumes(ts.port(), session.get()));
}

TEST(HttpTlsHotReload, InvalidTlsConfigUpdateIsRejected) {
  auto [certPem, keyPem] = CertKeyCache::Get().server;
  HttpServerConfig serverCfg;
  serverCfg.withTlsCertKeyMemory(certPem, keyPem);
  test::TestServer ts(std::move(serverCfg));

  // Rejected by validation: requiring client certificates needs a trust store.
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.withTlsRequireClientCert(true); });
  EXPECT_FALSE(ts.server.config().tls.requireClientCert);
  // Rejected by the TLS context: the private key does not match the certificate.
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.tls.withKeyPem(CertKeyCache::Get().client.second); });
  EXPECT_EQ(ts.server.config().tls.keyPem(), keyPem);

  EXPECT_EQ(NewConnectionCommonName(ts.port()), "server");
}

TEST(HttpTlsHandshakeTest, TrustStoreUpdateEnablesMutualTlsForNewConnections) {
  auto [serverCertPem, serverKeyPem] = CertKeyCache::Get().server;
  auto [clientCertPem, clientKeyPem] = CertKeyCache::Get().client;
  auto [otherCertPem, otherKeyPem] = CertKeyCache::Get().localhost;

  HttpServerConfig cfg;
  cfg.withTlsCertKeyMemory(serverCertPem, serverKeyPem);
  cfg.withTlsRequireClientCert(true);
  // Validation requires a non-empty trust store when requireClientCert=true.
  // Start with a non-matching trust store, then hot-swap to the real client cert.
  cfg.withTlsTrustedClientCert(otherCertPem);

  test::TestServer ts(std::move(cfg));

  ts.router().setDefault([](const HttpRequestView&) { return HttpResponse("OK"); });

  // Client presents a cert but server doesn't trust it yet -> handshake fails.
  {
    test::TlsClient::Options opt;
    opt.clientCertPem = clientCertPem;
    opt.clientKeyPem = clientKeyPem;
    test::TlsClient before(ts.port(), std::move(opt));
    ASSERT_TRUE(before.handshakeOk());
    auto resp = before.get("/", {});
    EXPECT_TRUE(resp.empty());
  }

  // Update trust store at runtime (synchronous — waits for the event loop to apply the change).
  ts.postConfigUpdate([clientCertPem](HttpServerConfig& cfg) {
    cfg.tls.withoutTlsTrustedClientCert().withTlsTrustedClientCert(clientCertPem);
  });

  {
    test::TlsClient::Options opt;
    opt.clientCertPem = clientCertPem;
    opt.clientKeyPem = clientKeyPem;
    test::TlsClient after(ts.port(), std::move(opt));
    ASSERT_TRUE(after.handshakeOk());
    auto resp = after.get("/", {});
    EXPECT_TRUE(resp.starts_with("HTTP/1.1 200"));
  }
}

TEST(HttpTlsHandshakeTest, SessionResumptionIncrementsResumedCounter) {
  // Enable session tickets so resumption is possible.
  test::TlsTestServer ts({}, [](HttpServerConfig& cfg) {
    cfg.tls.withTlsSessionTickets(true);
    cfg.tls.withTlsSessionTicketMaxKeys(2);
  });

  ts.setDefault([](const HttpRequestView&) { return HttpResponse("OK"); });

  test::TlsClient c1(ts.port());
  ASSERT_TRUE(c1.handshakeOk());
  // Drive post-handshake messages (TLS 1.3 NewSessionTicket) by doing a simple request.
  (void)c1.get("/", {});
  auto sess = c1.get1Session();
  ASSERT_NE(sess.get(), nullptr);

  test::TlsClient::Options opt;
  opt.reuseSession = sess.get();
  test::TlsClient c2(ts.port(), std::move(opt));
  ASSERT_TRUE(c2.handshakeOk());

  // The client handshake completing does not guarantee the server event loop already finalized the handshake
  // and updated TLS metrics. Poll until the resumed counter is visible (or timeout) to avoid flakiness.
  ServerStats st{};
  const auto deadline = std::chrono::steady_clock::now() + 500ms;
  while (std::chrono::steady_clock::now() < deadline) {
    st = ts.stats();
    if (st.tlsHandshakesResumed >= 1UL) {
      break;
    }
    std::this_thread::sleep_for(ts.server.server.config().pollInterval + 100us);
  }
  EXPECT_GE(st.tlsHandshakesFull, 1UL);
  EXPECT_GE(st.tlsHandshakesResumed, 1UL);
}

// Verifies that moving a TLS+ALPN configured SingleHttpServer prior to running preserves
// a valid TLS context and ALPN callback pointer. This specifically guards against
// the prior design where TlsContext was stored by value (e.g. inside std::optional):
// a move of SingleHttpServer could relocate the TlsContext object while the OpenSSL
// SSL_CTX ALPN selection callback still held the old address -> use-after-free /
// crash during handshake. The current design stores TlsContext behind a stable
// std::unique_ptr, so the address observed by OpenSSL remains valid after moves.
//
// This test would (non-deterministically) fail or ASan-crash under the old design
// when compiled with sanitizers and run enough times, especially under load, but
// here we simply assert successful handshake + ALPN negotiation after a move.
#ifdef AERONET_ENABLE_HTTP2
TEST(HttpTlsMoveAlpn, MoveConstructBeforeRunMaintainsAlpnHandshake) {
  auto pair = CertKeyCache::Get().localhost;
  ASSERT_FALSE(pair.first.empty());
  ASSERT_FALSE(pair.second.empty());

  HttpServerConfig cfg;
  cfg.withTlsCertKeyMemory(pair.first, pair.second);
  cfg.withTlsAlpnProtocols({"h2", "http/1.1"});  // offer both; client will request http/1.1 only
  cfg.withTlsRequireClientCert(false);           // no client cert for this test

  SingleHttpServer original(cfg);
  original.router().setDefault([](const HttpRequestView& req) {
    return HttpResponse(http::StatusCodeOK)
        .body(std::string("MOVEALPN:") + (req.alpnProtocol().empty() ? "-" : std::string(req.alpnProtocol())));
  });

  auto port = original.port();
  SingleHttpServer moved(std::move(original));

  std::atomic_bool stop{false};
  std::jthread th([&] { moved.runUntil([&] { return stop.load(); }); });

  // Actively wait until the listening socket accepts a plain TCP connection to avoid race.
  // This replicates TestServer readiness logic without duplicating its wrapper.
  {
    test::ClientConnection probe(port, std::chrono::milliseconds{500});
  }

  test::TlsClient::Options opts;
  opts.alpn = {"http/1.1"};
  test::TlsClient client(port, opts);
  ASSERT_TRUE(client.handshakeOk()) << "TLS handshake failed after move (potential stale TlsContext pointer)";
  auto raw = client.get("/moved");
  stop.store(true);

  ASSERT_TRUE(raw.starts_with("HTTP/1.1 200"));
  ASSERT_TRUE(raw.contains("MOVEALPN:http/1.1")) << raw;
}
#endif

// Test mutual TLS requirement and ALPN negotiation (server selects http/1.1)

TEST(HttpTlsMtlsAlpn, RequireClientCertHandshakeFailsWithout) {
  auto serverCert = CertKeyCache::Get().server;  // still needed for trust store
  ASSERT_FALSE(serverCert.first.empty());
  ASSERT_FALSE(serverCert.second.empty());
  std::string resp;
  std::string alpn;
  {
    test::TlsTestServer ts({"http/1.1"}, [&](HttpServerConfig& cfg) {
      cfg.withTlsRequireClientCert(true).withTlsTrustedClientCert(serverCert.first);
    });
    auto port = ts.port();
    ts.setDefault(
        [](const HttpRequestView& req) { return HttpResponse(std::string("SECURE") + std::string(req.path())); });
    test::TlsClient::Options opts;
    opts.alpn = {"http/1.1"};
    // No client cert provided, so handshake should fail due to required client cert.
    test::TlsClient client(port, opts);
    if (client.handshakeOk()) {
      resp = client.get("/secure");
      alpn = client.negotiatedAlpn();
    }
  }
  // Expect empty response (handshake failed or connection closed before HTTP response)
  ASSERT_TRUE(resp.empty());
}

// Regression: setting requireClientCert directly (mimicking JSON/glaze deserialization or raw field
// assignment) without the requestClientCert flag must still enforce mTLS. Before the fix the server
// never asked for a certificate in this configuration and accepted certificate-less clients.
TEST(HttpTlsMtlsAlpn, RequireClientCertDirectFieldHandshakeFailsWithout) {
  auto serverCert = CertKeyCache::Get().server;
  ASSERT_FALSE(serverCert.first.empty());
  ASSERT_FALSE(serverCert.second.empty());
  std::string resp;
  {
    test::TlsTestServer ts({"http/1.1"}, [&](HttpServerConfig& cfg) {
      // Deliberately bypass the builder's implication: set requireClientCert on the field directly,
      // leaving requestClientCert at its default (false). validate() must normalize the invariant.
      cfg.tls.requireClientCert = true;
      cfg.withTlsTrustedClientCert(serverCert.first);
      ASSERT_FALSE(cfg.tls.requestClientCert);
    });
    auto port = ts.port();
    ts.setDefault(
        [](const HttpRequestView& req) { return HttpResponse(std::string("SECURE") + std::string(req.path())); });
    test::TlsClient::Options opts;
    opts.alpn = {"http/1.1"};
    // No client cert provided, so the handshake must fail.
    test::TlsClient client(port, opts);
    if (client.handshakeOk()) {
      resp = client.get("/secure");
    }
  }
  ASSERT_TRUE(resp.empty());
}

TEST(HttpTlsMtlsAlpn, RequireClientCertSuccessWithAlpn) {
  auto serverCert = CertKeyCache::Get().server;
  ASSERT_FALSE(serverCert.first.empty());
  ASSERT_FALSE(serverCert.second.empty());
  auto clientCert = serverCert;  // reuse same self-signed for simplicity
  std::string resp;
  std::string alpn;
  {
    test::TlsTestServer ts({"http/1.1"}, [&](HttpServerConfig& cfg) {
      cfg.withTlsRequireClientCert(true).withTlsTrustedClientCert(clientCert.first);
    });
    auto port = ts.port();
    ts.setDefault(
        [](const HttpRequestView& req) { return HttpResponse(std::string("SECURE") + std::string(req.path())); });
    test::TlsClient::Options opts;
    opts.alpn = {"http/1.1"};
    opts.clientCertPem = clientCert.first;
    opts.clientKeyPem = clientCert.second;
    test::TlsClient client(port, opts);
    ASSERT_TRUE(client.handshakeOk());
    resp = client.get("/secure");
    alpn = client.negotiatedAlpn();
  }
  ASSERT_FALSE(resp.empty());
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 200"));
  ASSERT_TRUE(resp.contains("SECURE/secure"));
  ASSERT_EQ(alpn, "http/1.1");
}

TEST(HttpTlsCipherVersion, CipherAndVersionExposedAndMetricsIncrement) {
  // Metrics now per-server; no global reset needed.
  // TLS fixture auto-generates cert/key; request ALPN http/1.1
  std::string capturedCipher;
  std::string capturedVersion;
  std::string capturedAlpn;
  ServerStats statsSnapshot{};
  {
    test::TlsTestServer ts({"http/1.1"});
    auto port = ts.port();
    ts.setDefault([&](const HttpRequestView& req) {
      capturedCipher = std::string(req.tlsCipher());
      capturedVersion = std::string(req.tlsVersion());
      capturedAlpn = std::string(req.alpnProtocol());

      return HttpResponse("ok");
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(80));  // allow handshake path if needed
    test::TlsClient::Options opts;
    opts.alpn = {"http/1.1"};
    test::TlsClient client(port, opts);
    auto resp = client.get("/");
    ASSERT_TRUE(resp.starts_with("HTTP/1.1 200"));
    statsSnapshot = ts.stats();
    ASSERT_GE(statsSnapshot.tlsHandshakesSucceeded, 1U);
    ASSERT_EQ(statsSnapshot.tlsAlpnStrictMismatches, 0U);
  }
  ASSERT_FALSE(capturedCipher.empty());
  ASSERT_FALSE(capturedVersion.empty());
  // ALPN was offered and should match
  ASSERT_EQ(capturedAlpn, "http/1.1");
  // Distribution should show at least one for http/1.1
  bool found = false;
  for (const auto& kv : statsSnapshot.tlsAlpnDistribution) {
    if (kv.first == "http/1.1") {
      found = true;
      ASSERT_GE(kv.second, 1U);
    }
  }
  ASSERT_TRUE(found);
  ASSERT_EQ(statsSnapshot.tlsClientCertPresent, 0U);  // no client cert
  // Assert cipher & version distributions contain negotiated values
  bool cipherFound = false;
  for (const auto& kv : statsSnapshot.tlsCipherCounts) {
    if (kv.first == capturedCipher) {
      cipherFound = true;
      ASSERT_GE(kv.second, 1U);
    }
  }
  ASSERT_TRUE(cipherFound);
  bool versionFound = false;
  for (const auto& kv : statsSnapshot.tlsVersionCounts) {
    if (kv.first == capturedVersion) {
      versionFound = true;
      ASSERT_GE(kv.second, 1U);
    }
  }
  ASSERT_TRUE(versionFound);
  // Handshake duration metrics populated
  ASSERT_GE(statsSnapshot.tlsHandshakeDurationCount, 1U);
  ASSERT_GE(statsSnapshot.tlsHandshakeDurationTotalNs, statsSnapshot.tlsHandshakeDurationMaxNs);
  ASSERT_GT(statsSnapshot.tlsHandshakeDurationMaxNs, 0U);
}

TEST(HttpTlsFileCertKey, HandshakeSucceedsUsingFileBasedCertAndKey) {
  auto pair = CertKeyCache::Get().localhost;
  ASSERT_FALSE(pair.first.empty());
  ASSERT_FALSE(pair.second.empty());
  // Write both to temp files inside a temporary directory
  test::ScopedTempDir tmpDir;
  test::ScopedTempFile certFile(tmpDir, pair.first);
  test::ScopedTempFile keyFile(tmpDir, pair.second);

  HttpServerConfig cfg;
  cfg.withTlsCertKey(certFile.filePath().string(), keyFile.filePath().string());  // file-based path (not memory)
  cfg.withTlsAlpnProtocols({"http/1.1"});
  // Use plain TestServer since we manually set config
  test::TestServer server(cfg, RouterConfig(), std::chrono::milliseconds{50});
  server.router().setDefault([](const HttpRequestView& req) {
    return HttpResponse(std::string("FILETLS-") + std::string(req.alpnProtocol().empty() ? "-" : req.alpnProtocol()));
  });
  uint16_t port = server.port();

  test::TlsClient::Options opts;
  opts.alpn = {"http/1.1"};
  test::TlsClient client(port, opts);
  ASSERT_TRUE(client.handshakeOk());
  auto resp = client.get("/file");
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 200"));
  ASSERT_TRUE(resp.contains("FILETLS-http/1.1"));
}

TEST(HttpTlsMtlsMetrics, ClientCertPresenceIncrementsMetric) {
  // Per-server metrics now, no global reset required.
  auto certKey = CertKeyCache::Get().localhost;  // also used as trusted client CA

  test::TlsTestServer ts({"http/1.1"}, [&](HttpServerConfig& cfg) {
    cfg.withTlsRequireClientCert(true).withTlsTrustedClientCert(certKey.first);
  });
  auto port = ts.port();
  ts.setDefault([](const HttpRequestView&) { return HttpResponse("m"); });
  auto before = ts.stats();
  test::TlsClient::Options opts;
  opts.alpn = {"http/1.1"};
  opts.clientCertPem = certKey.first;
  opts.clientKeyPem = certKey.second;
  test::TlsClient client(port, opts);
  ASSERT_TRUE(client.handshakeOk());
  auto resp = client.get("/m");
  auto after = ts.stats();
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 200"));
  ASSERT_LT(before.tlsClientCertPresent, after.tlsClientCertPresent);
  ASSERT_GE(after.tlsHandshakesSucceeded, 1U);
}

TEST(HttpTlsSniCertificates, ExactHostPicksAlternateCertificate) {
  auto defaultPair = CertKeyCache::Get().localhost;
  auto sniPair = CertKeyCache::Get().server;

  HttpServerConfig cfg;
  cfg.withTlsCertKeyMemory(defaultPair.first, defaultPair.second);
  cfg.withTlsAlpnProtocols({"http/1.1"});
  cfg.tls.withTlsSniCertificateMemory("api.example.test", sniPair.first, sniPair.second);

  test::TestServer server(cfg, RouterConfig(), std::chrono::milliseconds{50});
  server.router().setDefault([](const HttpRequestView&) { return HttpResponse("SNI-EXACT"); });

  test::TlsClient::Options sniOpts;
  sniOpts.verifyPeer = true;
  sniOpts.alpn = {"http/1.1"};
  sniOpts.serverName = "api.example.test";
  sniOpts.trustedServerCertPem = sniPair.first;
  {
    test::TlsClient sniClient(server.port(), sniOpts);
    ASSERT_TRUE(sniClient.handshakeOk());
    auto resp = sniClient.get("/sni", {});
    ASSERT_TRUE(resp.starts_with("HTTP/1.1 200"));
    ASSERT_TRUE(resp.contains("SNI-EXACT"));
  }

  test::TlsClient::Options fallbackOpts = sniOpts;
  fallbackOpts.serverName.clear();
  test::TlsClient fallbackClient(server.port(), fallbackOpts);
  ASSERT_FALSE(fallbackClient.handshakeOk());

  server.server.stop();
  test::WaitForServer(server.server, false);
  auto moved = std::move(server);
  auto handler = moved.server.startDetached();
  ASSERT_TRUE(handler.started());

  {
    test::TlsClient sniClient(moved.port(), sniOpts);
    ASSERT_TRUE(sniClient.handshakeOk());
    auto resp = sniClient.get("/sni", {});
    ASSERT_TRUE(resp.starts_with("HTTP/1.1 200"));
    ASSERT_TRUE(resp.contains("SNI-EXACT"));
  }
}

TEST(HttpTlsSniCertificates, HttpsRedirectWithTlsThrows) {
  auto defaultPair = CertKeyCache::Get().localhost;
  auto wildcardPair = CertKeyCache::Get().server;

  HttpServerConfig cfg;
  cfg.withTlsCertKeyMemory(defaultPair.first, defaultPair.second);
  cfg.withTlsAlpnProtocols({"http/1.1"});
  cfg.tls.withTlsSniCertificateMemory("*.svc.test", wildcardPair.first, wildcardPair.second);
  EXPECT_NO_THROW(cfg.validate());
  cfg.withHttpsRedirect(8443, http::StatusCodePermanentRedirect);
  EXPECT_TRUE(cfg.httpsRedirect.enabled());
  EXPECT_THROW(cfg.validate(), std::invalid_argument);
}

TEST(HttpTlsSniCertificates, WildcardHostCaseInsensitiveMatch) {
  auto defaultPair = CertKeyCache::Get().localhost;
  auto wildcardPair = CertKeyCache::Get().server;

  HttpServerConfig cfg;
  cfg.withTlsCertKeyMemory(defaultPair.first, defaultPair.second);
  cfg.withTlsAlpnProtocols({"http/1.1"});
  cfg.tls.withTlsSniCertificateMemory("*.svc.test", wildcardPair.first, wildcardPair.second);

  test::TestServer server(cfg, RouterConfig(), std::chrono::milliseconds{50});
  server.router().setDefault([](const HttpRequestView&) { return HttpResponse("SNI-WILDCARD"); });

  test::TlsClient::Options wildcardOpts;
  wildcardOpts.verifyPeer = true;
  wildcardOpts.alpn = {"http/1.1"};
  wildcardOpts.serverName = "API.SVC.TEST";  // uppercase to exercise normalization
  wildcardOpts.trustedServerCertPem = wildcardPair.first;
  test::TlsClient wildcardClient(server.port(), wildcardOpts);
  ASSERT_TRUE(wildcardClient.handshakeOk());
  auto resp = wildcardClient.get("/wild", {});
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 200"));
  ASSERT_TRUE(resp.contains("SNI-WILDCARD"));

  test::TlsClient::Options missingOpts = wildcardOpts;
  missingOpts.serverName = "svc.test";  // no subdomain => should fall back to default cert
  test::TlsClient missingClient(server.port(), missingOpts);
  ASSERT_FALSE(missingClient.handshakeOk());
}

TEST(HttpTlsNegative, PlainHttpToTlsPortRejected) {
  // perform a raw TCP connect and send cleartext HTTP to a TLS-only port -> should fail handshake quickly.

  test::TlsTestServer ts;  // default TLS (no ALPN needed here)

  test::ClientConnection cnx(ts.port());
  auto fd = cnx.fd();

  std::string bogus = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";  // not TLS handshake
  test::sendAll(fd, bogus);
  // default recvWithTimeout waits 2000ms; explicit shorter timeout keeps test fast
  try {
    ASSERT_TRUE(test::recvWithTimeout(fd, 500ms).empty());
  } catch (const std::system_error& ex) {
    // Depending on timing and transport behavior, rejecting cleartext on a TLS port
    // may result in a reset (error::kConnectionReset) rather than an orderly close.
    const int err = ex.code().value();
    ASSERT_TRUE(err == error::kConnectionReset || err == error::kConnectionAborted || err == ENOTCONN) << ex.what();
  }
}

// When server only requests (but does not require) a client cert, handshake should succeed
// whether or not the client presents one; metric tlsClientCertPresent should reflect presence.

TEST(HttpTlsRequestClientCert, OptionalNoClientCertAccepted) {
  std::string body;
  ServerStats statsAfter{};
  {
    test::TlsTestServer ts({}, [](HttpServerConfig& cfg) { cfg.withTlsRequestClientCert(true); });
    auto port = ts.port();
    ts.setDefault([&](const HttpRequestView& req) {
      HttpResponse resp(200);
      if (!req.tlsCipher().empty()) {
        resp.body(std::string("REQ-") + std::string(req.tlsCipher()));
      } else {
        resp.body("REQ-");
      }
      return resp;
    });
    test::TlsClient client(port);  // no client cert
    ASSERT_TRUE(client.handshakeOk());
    body = client.get("/nocert");
    statsAfter = ts.stats();
  }
  ASSERT_TRUE(body.starts_with("HTTP/1.1 200"));
  ASSERT_EQ(statsAfter.tlsClientCertPresent, 0U);
  ASSERT_GE(statsAfter.tlsHandshakesSucceeded, 1U);
}

TEST(HttpTlsRequestClientCert, OptionalWithClientCertIncrementsMetric) {
  auto clientPair = CertKeyCache::Get().client;
  ASSERT_FALSE(clientPair.first.empty());
  ASSERT_FALSE(clientPair.second.empty());
  ServerStats statsAfter{};
  {
    // Trust the self-signed client cert for verification if sent; but handshake must still succeed w/out require flag.
    test::TlsTestServer ts({}, [&](HttpServerConfig& cfg) {
      cfg.withTlsRequestClientCert(true).withTlsTrustedClientCert(clientPair.first);
    });
    auto port = ts.port();
    ts.setDefault([](const HttpRequestView&) { return HttpResponse("C"); });
    test::TlsClient::Options opts;
    opts.clientCertPem = clientPair.first;
    opts.clientKeyPem = clientPair.second;
    test::TlsClient client(port, opts);
    ASSERT_TRUE(client.handshakeOk());
    auto response = client.get("/withcert");
    statsAfter = ts.stats();
    ASSERT_TRUE(response.starts_with("HTTP/1.1 200"));
  }
  ASSERT_GE(statsAfter.tlsHandshakesSucceeded, 1U);
  ASSERT_EQ(statsAfter.tlsClientCertPresent, 1U);
}

TEST(HttpTlsHandshakeTimeout, SilentClientClosed) {
  constexpr auto handshakeTimeout = std::chrono::milliseconds{40};
  test::TlsTestServer ts({}, [&](HttpServerConfig& cfg) {
    cfg.withTlsHandshakeTimeout(handshakeTimeout);
    cfg.withPollInterval(std::chrono::milliseconds{5});
  });

  test::ClientConnection cnx(ts.port());
  const auto fd = cnx.fd();
  ASSERT_GE(fd, 0) << "connect failed";

  const bool closed = test::WaitForPeerClose(fd, handshakeTimeout * 6);
  EXPECT_TRUE(closed);
}

TEST(HttpTlsHandshakeTimeout, SuccessfulHandshakeUnaffected) {
  test::TlsTestServer ts({},
                         [&](HttpServerConfig& cfg) { cfg.withTlsHandshakeTimeout(std::chrono::milliseconds{200}); });
  ts.setDefault([](const HttpRequestView&) { return HttpResponse("handshake-ok"); });

  test::TlsClient client(ts.port());
  ASSERT_TRUE(client.handshakeOk());
  const auto resp = client.get("/ok");

  ASSERT_TRUE(resp.starts_with("HTTP/1.1 200"));
  ASSERT_TRUE(resp.contains("handshake-ok"));
}

TEST(HttpTlsVersionBounds, MinMaxTls12Forces12) {
  std::string capturedVersion;
  ServerStats statsAfter{};
  {
    test::TlsTestServer ts({"http/1.1"},
                           [](HttpServerConfig& cfg) { cfg.withTlsMinVersion("TLS1.2").withTlsMaxVersion("TLS1.2"); });
    auto port = ts.port();
    ts.setDefault([&](const HttpRequestView& req) {
      if (!req.tlsVersion().empty()) {
        capturedVersion = std::string(req.tlsVersion());
      }

      return HttpResponse("V");
    });
    test::TlsClient::Options opts;
    opts.alpn = {"http/1.1"};
    test::TlsClient client(port, opts);
    ASSERT_TRUE(client.handshakeOk());
    auto resp = client.get("/v");
    statsAfter = ts.stats();
    ASSERT_TRUE(resp.starts_with("HTTP/1.1 200"));
  }
  ASSERT_FALSE(capturedVersion.empty());
  // OpenSSL commonly returns "TLSv1.2"; accept any token containing 1.2
  ASSERT_TRUE(capturedVersion.contains("1.2"));
  bool found = false;
  // Only iterate version counts if OpenSSL enabled (members present)
  for (const auto& kv : statsAfter.tlsVersionCounts) {
    if (kv.first == capturedVersion) {
      found = true;
      ASSERT_GE(kv.second, 1U);
    }
  }
  ASSERT_TRUE(found);
}

TEST(HttpTlsHandshakeTest, VerifiedClientCertificateSessionResumesWithItsSubject) {
  const auto& [serverCert, serverKey] = CertKeyCache::Get().localhost;
  const auto& [clientCert, clientKey] = CertKeyCache::Get().client;
  HttpServerConfig cfg;
  cfg.withTlsCertKeyMemory(serverCert, serverKey).withTlsRequireClientCert(true).withTlsTrustedClientCert(clientCert);
  cfg.tls.withTlsSessionTickets(true);
  test::TestServer ts(std::move(cfg));
  ts.router().setDefault([](const HttpRequestView& req) { return HttpResponse(req.tlsPeerSubject()); });

  test::TlsClient::Options opts;
  opts.clientCertPem = clientCert;
  opts.clientKeyPem = clientKey;
  test::TlsClient first(ts.port(), opts);
  ASSERT_TRUE(first.handshakeOk());
  const std::string firstResponse = first.get("/");
  ASSERT_TRUE(firstResponse.starts_with("HTTP/1.1 200"));
  EXPECT_TRUE(firstResponse.contains("CN=client")) << firstResponse;
  auto session = first.get1Session();
  ASSERT_NE(session.get(), nullptr);

  // Without a session id context, OpenSSL aborted the resumption of a session with a verified client certificate.
  opts.reuseSession = session.get();
  test::TlsClient second(ts.port(), opts);
  ASSERT_TRUE(second.handshakeOk());
  EXPECT_EQ(::SSL_session_reused(second.sslHandle()), 1);
  const std::string secondResponse = second.get("/");
  ASSERT_TRUE(secondResponse.starts_with("HTTP/1.1 200"));
  // The certificate of a resumed session is still known.
  EXPECT_TRUE(secondResponse.contains("CN=client")) << secondResponse;
}

TEST(HttpTlsHandshakeTest, PeerSubjectIsEmptyWithoutClientCertificate) {
  test::TlsTestServer ts;
  ts.setDefault(
      [](const HttpRequestView& req) { return HttpResponse(req.tlsPeerSubject().empty() ? "no-subject" : "subject"); });
  test::TlsClient client(ts.port());
  ASSERT_TRUE(client.handshakeOk());
  EXPECT_TRUE(client.get("/").ends_with("no-subject"));
}

TEST(HttpTlsHandshakeTest, OpenSslErrorLeftByAHandlerDoesNotCloseTheConnection) {
  test::TlsTestServer ts;
  ts.setDefault([](const HttpRequestView&) {
    // An error left on the error queue of the event loop thread, as a JWT check of a forged RSA signature would do.
    ERR_raise(ERR_LIB_RSA, ERR_R_INTERNAL_ERROR);
    return HttpResponse("handled");
  });
  test::TlsClient client(ts.port());
  ASSERT_TRUE(client.handshakeOk());

  // The read following the handler, that would block, must not be taken for a TLS failure (closing the connection).
  for (int requestIdx = 0; requestIdx < 3; ++requestIdx) {
    ASSERT_TRUE(client.writeAll("GET / HTTP/1.1\r\nHost: localhost\r\n\r\n")) << requestIdx;
    std::string response;
    char buf[1024];
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!response.ends_with("handled") && std::chrono::steady_clock::now() < deadline) {
      const auto chunk = client.readSome(buf);
      if (chunk.empty()) {
        std::this_thread::sleep_for(1ms);
      }
      response.append(chunk);
    }
    EXPECT_TRUE(response.starts_with("HTTP/1.1 200")) << requestIdx << ": " << response;
    EXPECT_TRUE(response.ends_with("handled")) << requestIdx;
  }
}

#ifdef AERONET_LINUX
namespace {

// A self-signed certificate carrying an unknown, non-critical extension of `extensionSize` bytes: a large certificate
// that stays cheap to parse.
std::string MakeLargeCertificatePem(std::size_t extensionSize) {
  PKeyPtr key(::EVP_EC_gen("P-256"), &::EVP_PKEY_free);
  X509Ptr cert(::X509_new(), &::X509_free);
  std::unique_ptr<ASN1_OBJECT, decltype(&::ASN1_OBJECT_free)> oid(::OBJ_txt2obj("1.3.6.1.4.1.99999.1", 1),
                                                                  &::ASN1_OBJECT_free);
  std::unique_ptr<ASN1_OCTET_STRING, decltype(&::ASN1_OCTET_STRING_free)> value(::ASN1_OCTET_STRING_new(),
                                                                                &::ASN1_OCTET_STRING_free);
  if (!key || !cert || !oid || !value) {
    throw std::runtime_error("Failed to allocate the large certificate");
  }
  ::X509_set_version(cert.get(), 2);
  ::ASN1_INTEGER_set(::X509_get_serialNumber(cert.get()), 1);
  ::X509_gmtime_adj(::X509_getm_notBefore(cert.get()), 0);
  ::X509_gmtime_adj(::X509_getm_notAfter(cert.get()), 3600);
  ::X509_set_pubkey(cert.get(), key.get());
  test::SetSelfIssuedName(cert.get(), "large");
  const std::string payload(extensionSize, 'x');
  std::unique_ptr<X509_EXTENSION, decltype(&::X509_EXTENSION_free)> extension(nullptr, &::X509_EXTENSION_free);
  if (::ASN1_OCTET_STRING_set(value.get(), reinterpret_cast<const unsigned char*>(payload.data()),
                              static_cast<int>(payload.size())) != 1 ||
      (extension.reset(::X509_EXTENSION_create_by_OBJ(nullptr, oid.get(), 0, value.get())), !extension) ||
      ::X509_add_ext(cert.get(), extension.get(), -1) != 1 || ::X509_sign(cert.get(), key.get(), ::EVP_sha256()) <= 0) {
    throw std::runtime_error("Failed to build the large certificate");
  }
  BioPtr bio(::BIO_new(::BIO_s_mem()), &::BIO_free);
  if (!bio || ::PEM_write_bio_X509(bio.get(), cert.get()) != 1) {
    throw std::runtime_error("Failed to write the large certificate");
  }
  char* data = nullptr;
  const long size = BIO_get_mem_data(bio.get(), &data);
  return {data, static_cast<std::size_t>(size)};
}

}  // namespace

TEST(HttpTlsHandshakeTest, HandshakeLargerThanTheSocketBuffersCompletes) {
  // A server flight (here, a huge certificate chain) larger than the socket buffers makes the handshake wait for the
  // socket to be writable. The writable event must resume the handshake: it used to only flush the (empty) output,
  // leaving the client waiting for the rest of the flight.
  const auto& [leafCert, leafKey] = CertKeyCache::Get().localhost;
  // 7 MiB: more than the socket send buffer can grow to (tcp_wmem max, 4 MiB by default) plus the client window. A
  // single large certificate rather than thousands of small ones: the client parses it quickly.
  const std::string chain = leafCert + MakeLargeCertificatePem(7UL << 20U);
  HttpServerConfig cfg;
  cfg.withTlsCertKeyMemory(chain, leafKey);
  // Once the server wrote its whole flight, the socket send buffer still holds megabytes that the kernel sends without
  // any event: the default timeouts could close the connection on a slow (sanitized) build before the client finishes.
  cfg.withKeepAliveTimeout(std::chrono::seconds{60}).withTlsHandshakeTimeout(std::chrono::seconds{60});
  test::TestServer ts(std::move(cfg));
  ts.router().setDefault([](const HttpRequestView&) { return HttpResponse("big-handshake"); });

  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(fd, 0);
  BaseFd socketFd(fd);
  // A receive buffer set before connecting: the advertised window stays bounded.
  const int rcvBuf = static_cast<int>(64U << 10U);
  // NOLINTNEXTLINE(misc-include-cleaner) sys/socket.h is the correct header for SOL_SOCKET
  ASSERT_EQ(::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvBuf, sizeof(rcvBuf)), 0);
  // Fails the test instead of hanging if the handshake stalls.
  const timeval timeout{.tv_sec = 30, .tv_usec = 0};  // NOLINT(misc-include-cleaner)
  // NOLINTNEXTLINE(misc-include-cleaner) sys/socket.h is the correct header for SOL_SOCKET
  ASSERT_EQ(::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)), 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(ts.port());
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  ASSERT_EQ(::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)), 0);

  SslCtxPtr clientCtx(::SSL_CTX_new(TLS_client_method()), &::SSL_CTX_free);
  ASSERT_NE(clientCtx, nullptr);
  ::SSL_CTX_set_verify(clientCtx.get(), SSL_VERIFY_NONE, nullptr);
  ::SSL_CTX_set_max_cert_list(clientCtx.get(), static_cast<long>(16UL << 20U));
  SslPtr ssl(::SSL_new(clientCtx.get()), &::SSL_free);
  ASSERT_NE(ssl, nullptr);
  ASSERT_EQ(::SSL_set_fd(ssl.get(), fd), 1);
  ASSERT_EQ(::SSL_connect(ssl.get()), 1);

  static constexpr std::string_view kRequest = "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
  ASSERT_EQ(::SSL_write(ssl.get(), kRequest.data(), static_cast<int>(kRequest.size())),
            static_cast<int>(kRequest.size()));
  std::string response;
  char buf[4096];
  for (int nbRead = ::SSL_read(ssl.get(), buf, sizeof(buf)); nbRead > 0;
       nbRead = ::SSL_read(ssl.get(), buf, sizeof(buf))) {
    response.append(buf, static_cast<std::size_t>(nbRead));
  }
  ::ERR_clear_error();
  EXPECT_TRUE(response.starts_with("HTTP/1.1 200")) << response;
  EXPECT_TRUE(response.ends_with("big-handshake"));
}
#endif
