#include "tls-handshake.hpp"

#include <openssl/asn1.h>
#include <openssl/bio.h>
#include <openssl/ssl.h>
#include <openssl/types.h>
#include <openssl/x509.h>

// Project headers (public API then internal RAII helpers)
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <utility>

#include "log.hpp"
#include "tls-metrics.hpp"
#include "tls-raii.hpp"

namespace aeronet {

TlsHandshakeResult collectTlsHandshakeInfo(const SSL* ssl, std::chrono::steady_clock::time_point handshakeStart) {
  TlsHandshakeResult res;
  if (ssl == nullptr) {
    return res;
  }
  // ALPN
  const unsigned char* sel = nullptr;
  unsigned int slen = 0;
  SSL_get0_alpn_selected(ssl, &sel, &slen);
  if ((sel != nullptr) && slen > 0) {
    res.selectedAlpn.assign(reinterpret_cast<const char*>(sel), slen);
  }
  if (const char* cipher = SSL_get_cipher_name(ssl)) {
    res.negotiatedCipher = cipher;
  }
  if (const char* vers = SSL_get_version(ssl)) {
    res.negotiatedVersion = vers;
  }
  if (X509* peerRaw = SSL_get_peer_certificate(ssl)) {
    X509Ptr peer(peerRaw, ::X509_free);
    res.clientCertPresent = true;
    if (X509_NAME* name = X509_get_subject_name(peer.get())) {
      auto memBio = makeMemoryBio();
      if (memBio) {
        if (X509_NAME_print_ex(memBio.get(), name, 0, XN_FLAG_RFC2253 & ~ASN1_STRFLGS_ESC_MSB) >= 0) {
          BUF_MEM* bptr = nullptr;
          BIO_get_mem_ptr(memBio.get(), &bptr);
          if ((bptr != nullptr) && bptr->length > 0) {
            res.peerSubject.assign(bptr->data, bptr->length);
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

TlsHandshakeResult collectAndLogTlsHandshake(const SSL* ssl, int fd, bool logHandshake,
                                             std::chrono::steady_clock::time_point handshakeStart) {
  auto res = collectTlsHandshakeInfo(ssl, handshakeStart);
  if (logHandshake) {
    log::info("TLS handshake fd={} ver={} cipher={} alpn={} peer={}", fd,
              res.negotiatedVersion.empty() ? "?" : res.negotiatedVersion.c_str(),
              res.negotiatedCipher.empty() ? "?" : res.negotiatedCipher.c_str(),
              res.selectedAlpn.empty() ? "-" : res.selectedAlpn.c_str(),
              res.peerSubject.empty() ? "-" : res.peerSubject.c_str());
  }
  return res;
}

void finalizeTlsHandshake(const SSL* ssl, int fd, bool logHandshake,
                          std::chrono::steady_clock::time_point handshakeStart, std::string& selectedAlpn,
                          std::string& negotiatedCipher, std::string& negotiatedVersion, TlsMetricsInternal& metrics) {
  auto hs = collectAndLogTlsHandshake(ssl, fd, logHandshake, handshakeStart);
  selectedAlpn = std::move(hs.selectedAlpn);
  negotiatedCipher = std::move(hs.negotiatedCipher);
  negotiatedVersion = std::move(hs.negotiatedVersion);
  // Metrics updates
  ++metrics.handshakesSucceeded;
  if (!selectedAlpn.empty()) {
    ++metrics.alpnDistribution[selectedAlpn];
  }
  if (hs.clientCertPresent) {
    ++metrics.clientCertPresent;
  }
  if (!negotiatedVersion.empty()) {
    ++metrics.versionCounts[negotiatedVersion];
  }
  if (!negotiatedCipher.empty()) {
    ++metrics.cipherCounts[negotiatedCipher];
  }
  if (hs.durationNs > 0) {
    ++metrics.handshakeDurationCount;
    metrics.handshakeDurationTotalNs += hs.durationNs;
    metrics.handshakeDurationMaxNs = std::max(metrics.handshakeDurationMaxNs, hs.durationNs);
  }
}

}  // namespace aeronet
