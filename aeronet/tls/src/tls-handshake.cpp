#include "aeronet/tls-handshake.hpp"

#include <openssl/asn1.h>
#include <openssl/bio.h>
#include <openssl/ssl.h>
#include <openssl/types.h>
#include <openssl/x509.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <utility>

#include "aeronet/log.hpp"
#include "aeronet/tls-config.hpp"
#include "aeronet/tls-handshake-callback.hpp"
#include "aeronet/tls-info.hpp"
#include "aeronet/tls-ktls.hpp"
#include "aeronet/tls-metrics.hpp"
#include "aeronet/tls-raii.hpp"

namespace aeronet {

namespace {

struct TlsHandshakeResult {
  // [0] - selectedAlpn - negotiated ALPN protocol (empty if none)
  // [1] - negotiatedCipher - negotiated TLS cipher suite
  // [2] - negotiatedVersion - negotiated TLS protocol version string
  // [3] - peerSubject - RFC2253 formatted subject if client cert present
  TLSInfo::Parts parts;
  bool clientCertPresent{false};
  uint64_t durationNs{0};  // handshake duration in nanoseconds (0 if start time unset)
};

// Collect negotiated TLS parameters and (optionally) peer subject. The handshakeStart timestamp
// should be the moment the TLS handshake began (steady clock). If it equals the epoch (count()==0)
// durationNs remains 0.
TlsHandshakeResult CollectTlsHandshakeInfo(const SSL* ssl, std::chrono::steady_clock::time_point handshakeStart) {
  TlsHandshakeResult res(TLSInfo::Parts(64U));

  // ALPN
  const unsigned char* sel = nullptr;
  unsigned int slen = 0;
  ::SSL_get0_alpn_selected(ssl, &sel, &slen);
  if (sel != nullptr) {
    res.parts.set(0, std::string_view(reinterpret_cast<const char*>(sel), slen));
  }
  if (const char* cipher = ::SSL_get_cipher_name(ssl)) {
    res.parts.set(1, cipher);
  }
  if (const char* vers = ::SSL_get_version(ssl)) {
    res.parts.set(2, vers);
  }

  if (X509* peerRaw = ::SSL_get_peer_certificate(ssl)) {
    auto peer = MakeX509(peerRaw);
    res.clientCertPresent = true;
    if (X509_NAME* name = ::X509_get_subject_name(peer.get())) {
      auto memBio = MakeMemoryBio();
      if (::X509_NAME_print_ex(memBio.get(), name, 0, XN_FLAG_RFC2253 & ~ASN1_STRFLGS_ESC_MSB) >= 0) {
        BUF_MEM* bptr = nullptr;
        ::BIO_get_mem_ptr(memBio.get(), &bptr);
        if (bptr != nullptr) {
          res.parts.set(3, std::string_view(bptr->data, bptr->length));
        }
      }
    }
  }

  if (handshakeStart.time_since_epoch().count() != 0) {
    auto dur = std::chrono::steady_clock::now() - handshakeStart;
    res.durationNs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(dur).count());
  }
  return res;
}

// Convenience: collect + optionally log in one call. Logging format aligns with server's prior implementation.
TlsHandshakeResult CollectAndLogTlsHandshake(const SSL* ssl, int fd, bool logHandshake,
                                             std::chrono::steady_clock::time_point handshakeStart) {
  auto res = CollectTlsHandshakeInfo(ssl, handshakeStart);
  if (logHandshake) {
    log::info("TLS handshake fd # {} ver={} cipher={} alpn={} peer={}", fd, res.parts[2], res.parts[1], res.parts[0],
              res.parts[3]);
  }
  return res;
}
}  // namespace

TLSInfo FinalizeTlsHandshake(const SSL* ssl, int fd, bool logHandshake, bool& tlsHandshakeEventEmitted,
                             const TlsHandshakeCallback& cb, std::chrono::steady_clock::time_point handshakeStart,
                             TlsMetricsInternal& metrics) {
  TlsHandshakeResult hs = CollectAndLogTlsHandshake(ssl, fd, logHandshake, handshakeStart);

  const bool resumed = ::SSL_session_reused(ssl) == 1;
  const bool clientCertPresent = hs.clientCertPresent;

  // Metrics updates
  ++metrics.handshakesSucceeded;
  if (resumed) {
    ++metrics.handshakesResumed;
  } else {
    ++metrics.handshakesFull;
  }
  if (clientCertPresent) {
    ++metrics.clientCertPresent;
  }

  TLSInfo tlsInfo{handshakeStart, std::move(hs.parts)};

  if (!tlsInfo.selectedAlpn().empty()) {
    auto [it, inserted] = metrics.alpnDistribution.emplace(tlsInfo.selectedAlpn(), 1);
    if (!inserted) {
      ++(it->second);
    }
  }
  if (!tlsInfo.negotiatedCipher().empty()) {
    auto [it, inserted] = metrics.cipherCounts.emplace(tlsInfo.negotiatedCipher(), 1);
    if (!inserted) {
      ++(it->second);
    }
  }
  if (!tlsInfo.negotiatedVersion().empty()) {
    auto [it, inserted] = metrics.versionCounts.emplace(tlsInfo.negotiatedVersion(), 1);
    if (!inserted) {
      ++(it->second);
    }
  }
  if (hs.durationNs > 0) {
    ++metrics.handshakeDurationCount;
    metrics.handshakeDurationTotalNs += hs.durationNs;
    metrics.handshakeDurationMaxNs = std::max(metrics.handshakeDurationMaxNs, hs.durationNs);
  }

  EmitTlsHandshakeEvent(tlsHandshakeEventEmitted, tlsInfo, cb, TlsHandshakeEvent::Result::Succeeded, fd, {}, resumed,
                        clientCertPresent);

  return tlsInfo;
}

namespace {

inline uint64_t DurationNs(std::chrono::steady_clock::time_point start) {
  if (start.time_since_epoch().count() == 0) {
    return 0;
  }
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
}

}  // namespace

void EmitTlsHandshakeEvent(bool& tlsHandshakeEventEmitted, const TLSInfo& tlsInfo, const TlsHandshakeCallback& cb,
                           TlsHandshakeEvent::Result result, int fd, std::string_view reason, bool resumed,
                           bool clientCertPresent) {
  if (tlsHandshakeEventEmitted) {
    return;
  }
  tlsHandshakeEventEmitted = true;

  if (cb) {
    TlsHandshakeEvent ev;
    ev.result = result;
    ev.reason = reason;
    ev.fd = fd;
    ev.resumed = resumed;
    ev.clientCertPresent = clientCertPresent;
    ev.durationNs = DurationNs(tlsInfo.handshakeStart);
    ev.selectedAlpn = tlsInfo.selectedAlpn();
    ev.negotiatedCipher = tlsInfo.negotiatedCipher();
    ev.negotiatedVersion = tlsInfo.negotiatedVersion();
    ev.peerSubject = tlsInfo.peerSubject();
    try {
      cb(ev);
    } catch (const std::exception& ex) {
      log::error("Exception raised in TLS handshake callback: {}", ex.what());
    } catch (...) {
      log::error("Unknown exception raised in TLS handshake callback");
    }
  }
}

KtlsApplication MaybeEnableKtlsSend(KtlsEnableResult ktlsResult, int fd, TLSConfig::KtlsMode ktlsMode,
                                    TlsMetricsInternal& metrics) {
  const bool force = ktlsMode == TLSConfig::KtlsMode::Required;
  const bool warnOnFailure = ktlsMode == TLSConfig::KtlsMode::Enabled || ktlsMode == TLSConfig::KtlsMode::Required;

  std::string_view reason = "unknown";
  switch (ktlsResult) {
    case KtlsEnableResult::Enabled:
      ++metrics.ktlsSendEnabledConnections;
      log::debug("kTLS send enabled on fd # {}", fd);
      return KtlsApplication::Enabled;
    case KtlsEnableResult::Unsupported:
      reason = "unsupported";
      [[fallthrough]];
    case KtlsEnableResult::Disabled:
      reason = "disabled";
      [[fallthrough]];
    default:
      ++metrics.ktlsSendEnableFallbacks;
      if (force) {
        ++metrics.ktlsSendForcedShutdowns;
        log::error("kTLS send {} on fd # {} while forced", reason, fd);
        return KtlsApplication::CloseConnection;
      }
      if (warnOnFailure) {
        log::warn("kTLS send {} on fd # {} (falling back to user-space TLS)", reason, fd);
      } else {
        log::debug("kTLS send {} on fd # {} (fallback)", reason, fd);
      }
      return KtlsApplication::Disabled;
  }
}

}  // namespace aeronet
