#include "aeronet/tls-handshake.hpp"

#include <openssl/asn1.h>
#include <openssl/bio.h>
#include <openssl/ssl.h>
#include <openssl/types.h>
#include <openssl/x509.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <utility>

#include "aeronet/log.hpp"
#include "aeronet/tls-info.hpp"
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
    X509Ptr peer(peerRaw, ::X509_free);
    res.clientCertPresent = true;
    if (X509_NAME* name = ::X509_get_subject_name(peer.get())) {
      auto memBio = makeMemoryBio();
      if (memBio) {
        if (::X509_NAME_print_ex(memBio.get(), name, 0, XN_FLAG_RFC2253 & ~ASN1_STRFLGS_ESC_MSB) >= 0) {
          BUF_MEM* bptr = nullptr;
          ::BIO_get_mem_ptr(memBio.get(), &bptr);
          if (bptr != nullptr) {
            res.parts.set(3, std::string_view(bptr->data, bptr->length));
          }
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

TLSInfo FinalizeTlsHandshake(const SSL* ssl, int fd, bool logHandshake,
                             std::chrono::steady_clock::time_point handshakeStart, TlsMetricsInternal& metrics) {
  TlsHandshakeResult hs = CollectAndLogTlsHandshake(ssl, fd, logHandshake, handshakeStart);

  TLSInfo ret{std::move(hs.parts)};

  // Metrics updates
  ++metrics.handshakesSucceeded;
  if (!ret.selectedAlpn().empty()) {
    auto [it, inserted] = metrics.alpnDistribution.emplace(ret.selectedAlpn(), 1);
    if (!inserted) {
      ++(it->second);
    }
  }
  if (hs.clientCertPresent) {
    ++metrics.clientCertPresent;
  }
  if (!ret.negotiatedCipher().empty()) {
    auto [it, inserted] = metrics.cipherCounts.emplace(ret.negotiatedCipher(), 1);
    if (!inserted) {
      ++(it->second);
    }
  }
  if (!ret.negotiatedVersion().empty()) {
    auto [it, inserted] = metrics.versionCounts.emplace(ret.negotiatedVersion(), 1);
    if (!inserted) {
      ++(it->second);
    }
  }
  if (hs.durationNs > 0) {
    ++metrics.handshakeDurationCount;
    metrics.handshakeDurationTotalNs += hs.durationNs;
    metrics.handshakeDurationMaxNs = std::max(metrics.handshakeDurationMaxNs, hs.durationNs);
  }

  return ret;
}

}  // namespace aeronet
