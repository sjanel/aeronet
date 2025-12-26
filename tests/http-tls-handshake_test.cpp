#include <gtest/gtest.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response-writer.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/router-config.hpp"
#include "aeronet/server-stats.hpp"
#include "aeronet/single-http-server.hpp"
#include "aeronet/temp-file.hpp"
#include "aeronet/test-tls-helper.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_server_tls_fixture.hpp"
#include "aeronet/test_tls_client.hpp"
#include "aeronet/test_util.hpp"

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
}  // namespace

TEST(HttpTlsAlpnMismatch, HandshakeFailsWhenNoCommonProtocolAndMustMatch) {
  test::TlsTestServer ts({"http/1.1", "h2"}, [](HttpServerConfig& cfg) {
    cfg.withTlsAlpnMustMatch(true);
    cfg.withTlsHandshakeLogging(true);
  });
  auto port = ts.port();
  ts.setDefault(
      [](const HttpRequest& req) { return HttpResponse(std::string("ALPN:") + std::string(req.alpnProtocol())); });
  // Offer only a mismatching ALPN; since TlsClient uses options, construct with protoX.
  test::TlsClient::Options opts;
  opts.alpn = {"protoX"};
  test::TlsClient client(port, opts);
  ServerStats statsAfter = ts.stats();

  ASSERT_FALSE(client.handshakeOk());
  ASSERT_GE(statsAfter.tlsAlpnStrictMismatches, 1U);
}

TEST(HttpTlsAlpnNonStrict, MismatchAllowedAndNoMetricIncrement) {
  std::string capturedAlpn;
  ServerStats statsAfter{};
  {
    // Server prefers h2, but does NOT enforce match.
    test::TlsTestServer ts({"h2"});
    auto port = ts.port();
    ts.setDefault([&](const HttpRequest& req) {
      if (!req.alpnProtocol().empty()) {
        capturedAlpn = std::string(req.alpnProtocol());
      } else {
        capturedAlpn.clear();
      }
      return HttpResponse(200, "OK").body("NS");
    });
    test::TlsClient::Options opts;
    opts.alpn = {"foo"};  // no overlap
    test::TlsClient client(port, opts);
    ASSERT_TRUE(client.handshakeOk());
    auto resp = client.get("/non_strict");
    statsAfter = ts.stats();
    ts.stop();
    ASSERT_TRUE(resp.contains("HTTP/1.1 200"));
  }
  // ALPN not negotiated => empty string
  ASSERT_TRUE(capturedAlpn.empty());
  ASSERT_EQ(statsAfter.tlsAlpnStrictMismatches, 0U);
  // Distribution should NOT contain client-only protocol
  for (const auto& kv : statsAfter.tlsAlpnDistribution) {
    ASSERT_NE(kv.first, std::string("foo"));
  }
}

extern std::atomic<int> g_aeronetTestFailNextSslNew;
extern std::atomic<int> g_aeronetTestFailNextSslSetFd;
namespace aeronet {
extern std::atomic<int> g_aeronetTestForceAlpnStrictMismatch;
}
using aeronet::g_aeronetTestForceAlpnStrictMismatch;

TEST(HttpTlsHandshakeCallback, ExceptionRaisedInCallbackIsLoggedAndIgnored) {
  test::TlsTestServer ts({"http/1.1"}, [](HttpServerConfig& cfg) {
    cfg.withTlsAlpnMustMatch(true);
    cfg.withTlsHandshakeLogging(true);
  });

  ts.server.server.setTlsHandshakeCallback([&]([[maybe_unused]] const SingleHttpServer::TlsHandshakeEvent& ev) {
    throw std::runtime_error("Simulated exception in handshake callback");
  });

  ts.setDefault([](const HttpRequest&) { return HttpResponse("OK"); });

  test::TlsClient::Options opt;
  opt.alpn = {"http/1.1"};
  test::TlsClient client(ts.port(), std::move(opt));
  ASSERT_TRUE(client.handshakeOk());
  auto resp = client.get("/", {});
  ASSERT_TRUE(resp.contains("HTTP/1.1 200"));
}

TEST(HttpTlsHandshakeCallback, UnknownExceptionRaisedInCallbackIsLoggedAndIgnored) {
  test::TlsTestServer ts({"http/1.1"}, [](HttpServerConfig& cfg) {
    cfg.withTlsAlpnMustMatch(true);
    cfg.withTlsHandshakeLogging(true);
  });

  ts.server.server.setTlsHandshakeCallback(
      [&]([[maybe_unused]] const SingleHttpServer::TlsHandshakeEvent& ev) { throw 42; });

  ts.setDefault([](const HttpRequest&) { return HttpResponse("OK"); });

  test::TlsClient::Options opt;
  opt.alpn = {"http/1.1"};
  test::TlsClient client(ts.port(), std::move(opt));
  ASSERT_TRUE(client.handshakeOk());
  auto resp = client.get("/", {});
  ASSERT_TRUE(resp.contains("HTTP/1.1 200"));
}

TEST(HttpTlsHandshakeCallback, EmitsSuccessEventWithNegotiatedAlpn) {
  test::TlsTestServer ts({"http/1.1"}, [](HttpServerConfig& cfg) {
    cfg.withTlsAlpnMustMatch(true);
    cfg.withTlsHandshakeLogging(true);
  });

  std::atomic<uint64_t> successCount{0};
  std::mutex mu;
  std::string lastAlpn;

  ts.server.server.setTlsHandshakeCallback([&](const SingleHttpServer::TlsHandshakeEvent& ev) {
    if (ev.result == SingleHttpServer::TlsHandshakeEvent::Result::Succeeded) {
      ++successCount;
      std::scoped_lock lock(mu);
      lastAlpn = std::string(ev.selectedAlpn);
    }
  });

  ts.setDefault([](const HttpRequest&) { return HttpResponse("OK"); });

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

TEST(HttpTlsHandshakeCallback, EmitsFailureEventAndBucketsReasonOnStrictAlpnMismatch) {
  test::TlsTestServer ts({"h2"}, [](HttpServerConfig& cfg) {
    cfg.withTlsAlpnMustMatch(true);
    cfg.withTlsHandshakeLogging(true);
  });

  std::atomic_bool callbackOK{false};

  ts.server.server.setTlsHandshakeCallback([&](const SingleHttpServer::TlsHandshakeEvent& ev) {
    if (ev.result == SingleHttpServer::TlsHandshakeEvent::Result::Failed && ev.reason == "alpn_strict_mismatch") {
      callbackOK.store(true);
    }
  });

  ts.setDefault([](const HttpRequest&) { return HttpResponse("OK"); });

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
      if (kv.first == "alpn_strict_mismatch" && kv.second >= 1) {
        found = true;
        break;
      }
    }
  }

  EXPECT_TRUE(callbackOK.load());
  EXPECT_TRUE(found);
}

TEST(HttpTlsHandshakeCallback, EmitsFailureEventWhenObserverFlagsAlpnStrictMismatchAfterSuccessfulHandshake) {
  // Force the observer flag without aborting the handshake to cover the post-handshake branch.
  g_aeronetTestForceAlpnStrictMismatch.store(1, std::memory_order_relaxed);

  test::TlsTestServer ts({"h2"}, [](HttpServerConfig& cfg) {
    cfg.withTlsAlpnMustMatch(true);
    cfg.withTlsHandshakeLogging(true);
  });

  std::atomic_bool callbackOK{false};
  std::string_view lastReason;
  SingleHttpServer::TlsHandshakeEvent::Result lastResult = SingleHttpServer::TlsHandshakeEvent::Result::Succeeded;

  ts.server.server.setTlsHandshakeCallback([&](const SingleHttpServer::TlsHandshakeEvent& ev) {
    lastResult = ev.result;
    lastReason = ev.reason;
    if (ev.result == SingleHttpServer::TlsHandshakeEvent::Result::Failed && ev.reason == "alpn_strict_mismatch") {
      callbackOK.store(true, std::memory_order_relaxed);
    }
  });

  ts.setDefault([](const HttpRequest&) { return HttpResponse("OK"); });

  test::TlsClient::Options opt;
  opt.alpn = {"h2"};  // ALPN intersects; handshake should succeed but observer is forced to flag a mismatch.
  test::TlsClient client(ts.port(), std::move(opt));
  ASSERT_TRUE(client.handshakeOk());

  // Wait for callback and stats propagation.
  ServerStats st{};
  bool found = false;
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (std::chrono::steady_clock::now() < deadline && (!callbackOK.load(std::memory_order_relaxed) || !found)) {
    std::this_thread::sleep_for(ts.server.server.config().pollInterval + 100us);
    st = ts.stats();
    for (const auto& kv : st.tlsHandshakeFailureReasons) {
      if (kv.first == "alpn_strict_mismatch" && kv.second >= 1) {
        found = true;
        break;
      }
    }
  }

  EXPECT_TRUE(callbackOK.load());
  EXPECT_EQ(lastResult, SingleHttpServer::TlsHandshakeEvent::Result::Failed);
  EXPECT_EQ(lastReason, "alpn_strict_mismatch");
  EXPECT_TRUE(found);
  EXPECT_GE(st.tlsAlpnStrictMismatches, 1U);
}

TEST(HttpTlsHandshakeCallback, EmitsRejectedEventAndBucketsReasonOnConcurrencyLimit) {
  test::TlsTestServer ts({}, [](HttpServerConfig& cfg) {
    cfg.tls.maxConcurrentHandshakes = 1;
    cfg.tls.handshakeTimeout = 5s;
  });

  std::atomic_bool callbackOK{false};
  std::string_view lastReason;

  ts.server.server.setTlsHandshakeCallback([&](const SingleHttpServer::TlsHandshakeEvent& ev) {
    if (ev.result == SingleHttpServer::TlsHandshakeEvent::Result::Rejected) {
      lastReason = ev.reason;
      callbackOK.store(true);
    }
  });

  ts.setDefault([](const HttpRequest&) { return HttpResponse("OK"); });

  // Hold one handshake slot open by connecting and not sending a TLS ClientHello.
  test::ClientConnection hold(ts.port());

  // Second connection should be rejected immediately.
  test::ClientConnection rejected(ts.port());
  EXPECT_TRUE(test::WaitForPeerClose(rejected.fd(), 500ms));

  // active wait for callback to be invoked
  const auto now = std::chrono::steady_clock::now();
  const auto deadline = now + 1s;
  while (std::chrono::steady_clock::now() < deadline && !callbackOK.load()) {
    std::this_thread::sleep_for(1ms);
  }
  const auto st = ts.stats();

  EXPECT_TRUE(callbackOK.load());
  EXPECT_EQ(lastReason, "rejected_concurrency");
  EXPECT_GE(st.tlsHandshakesRejectedConcurrency, 1UL);

  bool found = false;
  for (const auto& kv : st.tlsHandshakeFailureReasons) {
    if (kv.first == "rejected_concurrency") {
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

  ts.server.server.setTlsHandshakeCallback([&](const SingleHttpServer::TlsHandshakeEvent& ev) {
    if (ev.result == SingleHttpServer::TlsHandshakeEvent::Result::Rejected) {
      lastReason = ev.reason;
      callbackOK.store(true);
    }
  });

  ts.setDefault([](const HttpRequest&) { return HttpResponse("OK"); });

  // First connection consumes the single token.
  test::ClientConnection first(ts.port());

  // Second connection in the same second should be rejected.
  test::ClientConnection rejected(ts.port());
  EXPECT_TRUE(test::WaitForPeerClose(rejected.fd(), 500ms));

  const auto now = std::chrono::steady_clock::now();
  const auto deadline = now + 1s;
  while (std::chrono::steady_clock::now() < deadline && !callbackOK.load()) {
    std::this_thread::sleep_for(1ms);
  }
  const auto st = ts.stats();

  EXPECT_EQ(lastReason, "rejected_rate_limit");
  EXPECT_GE(st.tlsHandshakesRejectedRateLimit, 1UL);

  bool found = false;
  for (const auto& kv : st.tlsHandshakeFailureReasons) {
    if (kv.first == "rejected_rate_limit") {
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

  ts.server.server.setTlsHandshakeCallback([&](const SingleHttpServer::TlsHandshakeEvent& ev) {
    if (ev.result == SingleHttpServer::TlsHandshakeEvent::Result::Failed) {
      lastReason = ev.reason;
      callbackOK.store(true);
    }
  });

  ts.setDefault([](const HttpRequest&) { return HttpResponse("OK"); });

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
  EXPECT_EQ(lastReason, "handshake_timeout");
  EXPECT_GE(st.tlsHandshakesFailed, 1UL);

  bool found = false;
  for (const auto& kv : st.tlsHandshakeFailureReasons) {
    if (kv.first == "handshake_timeout") {
      found = true;
      EXPECT_GE(kv.second, 1UL);
    }
  }
  EXPECT_TRUE(found);
}

TEST(HttpTlsHandshakeCallback, BucketsReasonWhenSslNewFails) {
  test::TlsTestServer ts;

  std::atomic_bool callbackOK{false};

  ts.server.server.setTlsHandshakeCallback([&](const SingleHttpServer::TlsHandshakeEvent& ev) {
    if (ev.result == SingleHttpServer::TlsHandshakeEvent::Result::Failed && ev.reason == "ssl_new_failed") {
      callbackOK.store(true);
    }
  });

  ts.setDefault([](const HttpRequest&) { return HttpResponse("OK"); });

  g_aeronetTestFailNextSslNew.store(1, std::memory_order_relaxed);
  {
    test::ClientConnection cnx(ts.port());
    // failure triggered; callback will be observed via active wait
  }

  // Active wait for callback to be invoked (deadline-style)
  const auto now = std::chrono::steady_clock::now();
  const auto deadline = now + 1s;
  while (std::chrono::steady_clock::now() < deadline && !callbackOK.load()) {
    std::this_thread::sleep_for(1ms);
  }
  const auto st = ts.stats();
  ts.stop();

  EXPECT_TRUE(callbackOK.load());

  bool found = false;
  for (const auto& kv : st.tlsHandshakeFailureReasons) {
    if (kv.first == "ssl_new_failed") {
      found = true;
      EXPECT_GE(kv.second, 1UL);
    }
  }
  EXPECT_TRUE(found);
}

TEST(HttpTlsHandshakeCallback, RefillsRateLimitAfterInterval) {
  test::TlsTestServer ts({}, [](HttpServerConfig& cfg) {
    cfg.tls.maxConcurrentHandshakes = 0;  // avoid concurrency interference
    cfg.tls.handshakeRateLimitPerSecond = 1;
    cfg.tls.handshakeRateLimitBurst = 1;
    cfg.tls.handshakeTimeout = 500ms;
  });

  ts.setDefault([](const HttpRequest&) { return HttpResponse("OK"); });

  // Consume the single token
  test::ClientConnection first(ts.port());
  const auto firstHandshakeFinished = std::chrono::steady_clock::now();
  // Wait slightly more than one second to ensure the seconds-based refill calculation
  // yields addIntervals > 0 in connection-manager.cpp (which uses whole-second intervals).
  const auto waitUntil = firstHandshakeFinished + 1100ms;

  // Second connection in same second should be rejected
  test::ClientConnection rejected(ts.port());
  EXPECT_TRUE(test::WaitForPeerClose(rejected.fd(), 500ms));

  // Wait more than one second so refill happens (addIntervals > 0)
  const auto now = std::chrono::steady_clock::now();
  if (now < waitUntil) {
    std::this_thread::sleep_for(waitUntil - now);
  }

  // Now a new connection should be accepted because tokens were refilled
  test::ClientConnection after(ts.port());
  // If connection is not closed immediately, assume accepted
  EXPECT_FALSE(test::WaitForPeerClose(after.fd(), 250ms));
  ts.stop();
}

TEST(HttpTlsHandshakeCallback, BucketsReasonWhenSslSetFdFails) {
  test::TlsTestServer ts;

  std::atomic_bool callbackOK{false};

  ts.server.server.setTlsHandshakeCallback([&](const SingleHttpServer::TlsHandshakeEvent& ev) {
    if (ev.result == SingleHttpServer::TlsHandshakeEvent::Result::Failed && ev.reason == "ssl_set_fd_failed") {
      callbackOK.store(true);
    }
  });

  ts.setDefault([](const HttpRequest&) { return HttpResponse(http::StatusCodeOK).body("OK"); });

  g_aeronetTestFailNextSslSetFd.store(1, std::memory_order_relaxed);
  {
    test::ClientConnection cnx(ts.port());
    // failure triggered; callback will be observed via active wait
  }

  // Active wait for callback to be invoked (deadline-style)
  const auto now = std::chrono::steady_clock::now();
  const auto deadline = now + 1s;
  while (std::chrono::steady_clock::now() < deadline && !callbackOK.load()) {
    std::this_thread::sleep_for(1ms);
  }
  const auto st = ts.stats();
  ts.stop();

  EXPECT_TRUE(callbackOK.load());

  bool found = false;
  for (const auto& kv : st.tlsHandshakeFailureReasons) {
    if (kv.first == "ssl_set_fd_failed") {
      found = true;
      EXPECT_GE(kv.second, 1UL);
    }
  }
  EXPECT_TRUE(found);
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

  ts.router().setDefault([](const HttpRequest&) { return HttpResponse("OK"); });

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

  // Update trust store at runtime.
  ts.server.postConfigUpdate([clientCertPem](HttpServerConfig& cfg) {
    cfg.tls.withoutTlsTrustedClientCert().withTlsTrustedClientCert(clientCertPem);
  });
  std::this_thread::sleep_for(ts.server.config().pollInterval + 100us);

  {
    test::TlsClient::Options opt;
    opt.clientCertPem = clientCertPem;
    opt.clientKeyPem = clientKeyPem;
    test::TlsClient after(ts.port(), std::move(opt));
    ASSERT_TRUE(after.handshakeOk());
    auto resp = after.get("/", {});
    EXPECT_TRUE(resp.contains("HTTP/1.1 200"));
  }
}

TEST(HttpTlsHandshakeTest, SessionResumptionIncrementsResumedCounter) {
  // Enable session tickets so resumption is possible.
  test::TlsTestServer ts({}, [](HttpServerConfig& cfg) {
    cfg.tls.withTlsSessionTickets(true);
    cfg.tls.withTlsSessionTicketMaxKeys(2);
  });

  ts.setDefault([](const HttpRequest&) { return HttpResponse("OK"); });

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

TEST(HttpTlsMoveAlpn, MoveConstructBeforeRunMaintainsAlpnHandshake) {
  auto pair = CertKeyCache::Get().localhost;
  ASSERT_FALSE(pair.first.empty());
  ASSERT_FALSE(pair.second.empty());

  HttpServerConfig cfg;
  cfg.withTlsCertKeyMemory(pair.first, pair.second);
  cfg.withTlsAlpnProtocols({"h2", "http/1.1"});  // offer both; client will request http/1.1 only
  cfg.withTlsRequireClientCert(false);           // no client cert for this test

  SingleHttpServer original(cfg);
  original.router().setDefault([](const HttpRequest& req) {
    return HttpResponse(http::StatusCodeOK, "OK")
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

  ASSERT_TRUE(raw.contains("HTTP/1.1 200"));
  ASSERT_TRUE(raw.contains("MOVEALPN:http/1.1")) << raw;
}

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
    ts.setDefault([](const HttpRequest& req) { return HttpResponse(std::string("SECURE") + std::string(req.path())); });
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
    ts.setDefault([](const HttpRequest& req) { return HttpResponse(std::string("SECURE") + std::string(req.path())); });
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
  ASSERT_TRUE(resp.contains("HTTP/1.1 200"));
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
    ts.setDefault([&](const HttpRequest& req) {
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
    ASSERT_TRUE(resp.contains("HTTP/1.1 200"));
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

TEST(HttpTlsCipherList, InvalidCipherListThrows) {
  EXPECT_THROW(
      { test::TlsTestServer ts({}, [](HttpServerConfig& cfg) { cfg.withTlsCipherList("INVALID-CIPHER-1234"); }); },
      std::runtime_error);
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
  test::TestServer server(cfg, RouterConfig{}, std::chrono::milliseconds{50});
  server.router().setDefault([](const HttpRequest& req) {
    return HttpResponse(std::string("FILETLS-") + std::string(req.alpnProtocol().empty() ? "-" : req.alpnProtocol()));
  });
  uint16_t port = server.port();

  test::TlsClient::Options opts;
  opts.alpn = {"http/1.1"};
  test::TlsClient client(port, opts);
  ASSERT_TRUE(client.handshakeOk());
  auto resp = client.get("/file");
  ASSERT_TRUE(resp.contains("HTTP/1.1 200"));
  ASSERT_TRUE(resp.contains("FILETLS-http/1.1"));
}

TEST(HttpTlsMtlsMetrics, ClientCertPresenceIncrementsMetric) {
  // Per-server metrics now, no global reset required.
  auto certKey = CertKeyCache::Get().localhost;  // also used as trusted client CA
  ASSERT_FALSE(certKey.first.empty());
  ASSERT_FALSE(certKey.second.empty());
  {
    test::TlsTestServer ts({"http/1.1"}, [&](HttpServerConfig& cfg) {
      cfg.withTlsRequireClientCert(true).withTlsTrustedClientCert(certKey.first);
    });
    auto port = ts.port();
    ts.setDefault([](const HttpRequest&) { return HttpResponse("m"); });
    auto before = ts.stats();
    test::TlsClient::Options opts;
    opts.alpn = {"http/1.1"};
    opts.clientCertPem = certKey.first;
    opts.clientKeyPem = certKey.second;
    test::TlsClient client(port, opts);
    ASSERT_TRUE(client.handshakeOk());
    auto resp = client.get("/m");
    auto after = ts.stats();
    ASSERT_TRUE(resp.contains("HTTP/1.1 200"));
    ASSERT_LT(before.tlsClientCertPresent, after.tlsClientCertPresent);
    ASSERT_GE(after.tlsHandshakesSucceeded, 1U);
  }
}

TEST(HttpTlsSniCertificates, ExactHostPicksAlternateCertificate) {
  auto defaultPair = CertKeyCache::Get().localhost;
  auto sniPair = CertKeyCache::Get().server;

  HttpServerConfig cfg;
  cfg.withTlsCertKeyMemory(defaultPair.first, defaultPair.second);
  cfg.withTlsAlpnProtocols({"http/1.1"});
  cfg.tls.withTlsSniCertificateMemory("api.example.test", sniPair.first, sniPair.second);

  test::TestServer server(cfg, RouterConfig{}, std::chrono::milliseconds{50});
  server.router().setDefault([](const HttpRequest&) { return HttpResponse("SNI-EXACT"); });

  test::TlsClient::Options sniOpts;
  sniOpts.verifyPeer = true;
  sniOpts.alpn = {"http/1.1"};
  sniOpts.serverName = "api.example.test";
  sniOpts.trustedServerCertPem = sniPair.first;
  test::TlsClient sniClient(server.port(), sniOpts);
  ASSERT_TRUE(sniClient.handshakeOk());
  auto resp = sniClient.get("/sni", {});
  ASSERT_TRUE(resp.contains("HTTP/1.1 200"));
  ASSERT_TRUE(resp.contains("SNI-EXACT"));

  test::TlsClient::Options fallbackOpts = sniOpts;
  fallbackOpts.serverName.clear();
  test::TlsClient fallbackClient(server.port(), fallbackOpts);
  ASSERT_FALSE(fallbackClient.handshakeOk());
}

TEST(HttpTlsSniCertificates, WildcardHostCaseInsensitiveMatch) {
  auto defaultPair = CertKeyCache::Get().localhost;
  auto wildcardPair = CertKeyCache::Get().server;

  HttpServerConfig cfg;
  cfg.withTlsCertKeyMemory(defaultPair.first, defaultPair.second);
  cfg.withTlsAlpnProtocols({"http/1.1"});
  cfg.tls.withTlsSniCertificateMemory("*.svc.test", wildcardPair.first, wildcardPair.second);

  test::TestServer server(cfg, RouterConfig{}, std::chrono::milliseconds{50});
  server.router().setDefault([](const HttpRequest&) { return HttpResponse("SNI-WILDCARD"); });

  test::TlsClient::Options wildcardOpts;
  wildcardOpts.verifyPeer = true;
  wildcardOpts.alpn = {"http/1.1"};
  wildcardOpts.serverName = "API.SVC.TEST";  // uppercase to exercise normalization
  wildcardOpts.trustedServerCertPem = wildcardPair.first;
  test::TlsClient wildcardClient(server.port(), wildcardOpts);
  ASSERT_TRUE(wildcardClient.handshakeOk());
  auto resp = wildcardClient.get("/wild", {});
  ASSERT_TRUE(resp.contains("HTTP/1.1 200"));
  ASSERT_TRUE(resp.contains("SNI-WILDCARD"));

  test::TlsClient::Options missingOpts = wildcardOpts;
  missingOpts.serverName = "svc.test";  // no subdomain => should fall back to default cert
  test::TlsClient missingClient(server.port(), missingOpts);
  ASSERT_FALSE(missingClient.handshakeOk());
}

namespace {
// Large response GET using TlsClient (simplified replacement).
std::string tlsGetLarge(auto port) {
  test::TlsClient client(port);
  if (!client.handshakeOk()) {
    return {};
  }
  return client.get("/large");
}
}  // namespace

TEST(HttpTlsNegative, PlainHttpToTlsPortRejected) {
  // perform a raw TCP connect and send cleartext HTTP to a TLS-only port -> should fail handshake quickly.

  test::TlsTestServer ts;  // default TLS (no ALPN needed here)

  test::ClientConnection cnx(ts.port());
  int fd = cnx.fd();

  std::string bogus = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";  // not TLS handshake
  test::sendAll(fd, bogus);
  // default recvWithTimeout waits 2000ms; explicit shorter timeout keeps test fast
  try {
    ASSERT_TRUE(test::recvWithTimeout(fd, 500ms).empty());
  } catch (const std::system_error& ex) {
    // Depending on timing and transport behavior, rejecting cleartext on a TLS port
    // may result in a reset (ECONNRESET) rather than an orderly close.
    const int err = ex.code().value();
    ASSERT_TRUE(err == ECONNRESET || err == ECONNABORTED || err == ENOTCONN) << ex.what();
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
    ts.setDefault([&](const HttpRequest& req) {
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
  ASSERT_TRUE(body.contains("HTTP/1.1 200"));
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
    ts.setDefault([](const HttpRequest&) { return HttpResponse("C"); });
    test::TlsClient::Options opts;
    opts.clientCertPem = clientPair.first;
    opts.clientKeyPem = clientPair.second;
    test::TlsClient client(port, opts);
    ASSERT_TRUE(client.handshakeOk());
    auto response = client.get("/withcert");
    statsAfter = ts.stats();
    ASSERT_TRUE(response.contains("HTTP/1.1 200"));
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
  const int fd = cnx.fd();
  ASSERT_GE(fd, 0) << "connect failed";

  const bool closed = test::WaitForPeerClose(fd, handshakeTimeout * 6);
  EXPECT_TRUE(closed);
}

TEST(HttpTlsHandshakeTimeout, SuccessfulHandshakeUnaffected) {
  test::TlsTestServer ts({},
                         [&](HttpServerConfig& cfg) { cfg.withTlsHandshakeTimeout(std::chrono::milliseconds{200}); });
  ts.setDefault([](const HttpRequest&) { return HttpResponse("handshake-ok"); });

  test::TlsClient client(ts.port());
  ASSERT_TRUE(client.handshakeOk());
  const auto resp = client.get("/ok");

  ASSERT_TRUE(resp.contains("HTTP/1.1 200"));
  ASSERT_TRUE(resp.contains("handshake-ok"));
}

TEST(HttpTlsVersionBounds, MinMaxTls12Forces12) {
  std::string capturedVersion;
  ServerStats statsAfter{};
  {
    test::TlsTestServer ts({"http/1.1"},
                           [](HttpServerConfig& cfg) { cfg.withTlsMinVersion("TLS1.2").withTlsMaxVersion("TLS1.2"); });
    auto port = ts.port();
    ts.setDefault([&](const HttpRequest& req) {
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
    ASSERT_TRUE(resp.contains("HTTP/1.1 200"));
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
