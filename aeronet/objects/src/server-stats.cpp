#include "aeronet/server-stats.hpp"

#include <cassert>
#include <charconv>
#include <concepts>
#include <cstddef>
#include <string>
#include <string_view>

#include "aeronet/nchars.hpp"

namespace aeronet {

namespace {

void Append(std::string& out, std::integral auto value) {
  const std::size_t oldSize = out.size();
  const std::size_t width = nchars(value);
  out.resize_and_overwrite(oldSize + width, [oldSize, width, value](char* data, std::size_t /*n*/) {
    std::to_chars(data + oldSize, data + oldSize + width, value);
    return oldSize + width;
  });
}

// ---- Static json fragments -------------------------------------------

constexpr std::string_view kOpen = "{";
constexpr std::string_view kClose = "}";
constexpr std::string_view kComma = ",";

constexpr std::string_view kTotalBytesQueued = R"("totalBytesQueued":)";
constexpr std::string_view kTotalBytesWrittenImmediate = R"(,"totalBytesWrittenImmediate":)";
constexpr std::string_view kTotalBytesWrittenFlush = R"(,"totalBytesWrittenFlush":)";
constexpr std::string_view kDeferredWriteEvents = R"(,"deferredWriteEvents":)";
constexpr std::string_view kFlushCycles = R"(,"flushCycles":)";
constexpr std::string_view kEpollModFailures = R"(,"epollModFailures":)";
constexpr std::string_view kMaxConnectionOutboundBuffer = R"(,"maxConnectionOutboundBuffer":)";
constexpr std::string_view kTotalRequestsServed = R"(,"totalRequestsServed":)";

#ifdef AERONET_ENABLE_OPENSSL
constexpr std::string_view kKtlsSendEnabledConnections = R"(,"ktlsSendEnabledConnections":)";
constexpr std::string_view kKtlsSendEnableFallbacks = R"(,"ktlsSendEnableFallbacks":)";
constexpr std::string_view kKtlsSendForcedShutdowns = R"(,"ktlsSendForcedShutdowns":)";
constexpr std::string_view kKtlsSendBytes = R"(,"ktlsSendBytes":)";
constexpr std::string_view kTlsHandshakesSucceeded = R"(,"tlsHandshakesSucceeded":)";
constexpr std::string_view kTlsHandshakesFull = R"(,"tlsHandshakesFull":)";
constexpr std::string_view kTlsHandshakesResumed = R"(,"tlsHandshakesResumed":)";
constexpr std::string_view kTlsHandshakesFailed = R"(,"tlsHandshakesFailed":)";
constexpr std::string_view kTlsHandshakesRejectedConcurrency = R"(,"tlsHandshakesRejectedConcurrency":)";
constexpr std::string_view kTlsHandshakesRejectedRateLimit = R"(,"tlsHandshakesRejectedRateLimit":)";
constexpr std::string_view kTlsClientCertPresent = R"(,"tlsClientCertPresent":)";
constexpr std::string_view kTlsAlpnStrictMismatches = R"(,"tlsAlpnStrictMismatches":)";

constexpr std::string_view kTlsAlpnDistribution = R"(,"tlsAlpnDistribution":[)";
constexpr std::string_view kTlsHandshakeFailureReasons = R"(,"tlsHandshakeFailureReasons":[)";
constexpr std::string_view kTlsVersionCounts = R"(,"tlsVersionCounts":[)";
constexpr std::string_view kTlsCipherCounts = R"(,"tlsCipherCounts":[)";
constexpr std::string_view kArrayClose = "]";

constexpr std::string_view kProtocolPrefix = R"({"protocol":")";
constexpr std::string_view kReasonPrefix = R"({"reason":")";
constexpr std::string_view kVersionPrefix = R"({"version":")";
constexpr std::string_view kCipherPrefix = R"({"cipher":")";
constexpr std::string_view kCountInfix = R"(","count":)";
constexpr std::string_view kEntryClose = "}";

constexpr std::string_view kTlsHandshakeDurationCount = R"(,"tlsHandshakeDurationCount":)";
constexpr std::string_view kTlsHandshakeDurationTotalNs = R"(,"tlsHandshakeDurationTotalNs":)";
constexpr std::string_view kTlsHandshakeDurationMaxNs = R"(,"tlsHandshakeDurationMaxNs":)";

// Exact size of an entry `{"<label>":"<key>","count":<n>}`.
std::size_t EntrySize(std::string_view prefix, std::string_view key, std::integral auto count) {
  return prefix.size() + key.size() + kCountInfix.size() + nchars(count) + kEntryClose.size();
}

// Exact size of the complete JSON array (bounds + separators + entries).
template <typename Map>
std::size_t MapSize(std::string_view arrayOpen, std::string_view entryPrefix, const Map& map) {
  std::size_t size = arrayOpen.size() + kArrayClose.size();
  bool first = true;
  for (const auto& kv : map) {
    if (!first) {
      size += kComma.size();
    }
    first = false;
    size += EntrySize(entryPrefix, kv.first, kv.second);
  }
  return size;
}

// Writes the complete JSON array. Exact mirror of MapSize above.
template <typename Map>
void AppendMap(std::string& out, std::string_view arrayOpen, std::string_view entryPrefix, const Map& map) {
  out.append(arrayOpen);
  bool first = true;
  for (const auto& kv : map) {
    if (!first) {
      out.append(kComma);
    }
    first = false;
    out.append(entryPrefix);
    out.append(kv.first);
    out.append(kCountInfix);
    Append(out, kv.second);
    out.append(kEntryClose);
  }
  out.append(kArrayClose);
}
#endif

}  // namespace

std::string ServerStats::json_str() const {
  // ---- First pass - compute exact size of the json str -----------------------------
  std::size_t size = kOpen.size() + kClose.size();
  size += kTotalBytesQueued.size() + nchars(totalBytesQueued);
  size += kTotalBytesWrittenImmediate.size() + nchars(totalBytesWrittenImmediate);
  size += kTotalBytesWrittenFlush.size() + nchars(totalBytesWrittenFlush);
  size += kDeferredWriteEvents.size() + nchars(deferredWriteEvents);
  size += kFlushCycles.size() + nchars(flushCycles);
  size += kEpollModFailures.size() + nchars(epollModFailures);
  size += kMaxConnectionOutboundBuffer.size() + nchars(maxConnectionOutboundBuffer);
  size += kTotalRequestsServed.size() + nchars(totalRequestsServed);

#ifdef AERONET_ENABLE_OPENSSL
  size += kKtlsSendEnabledConnections.size() + nchars(ktlsSendEnabledConnections);
  size += kKtlsSendEnableFallbacks.size() + nchars(ktlsSendEnableFallbacks);
  size += kKtlsSendForcedShutdowns.size() + nchars(ktlsSendForcedShutdowns);
  size += kKtlsSendBytes.size() + nchars(ktlsSendBytes);
  size += kTlsHandshakesSucceeded.size() + nchars(tlsHandshakesSucceeded);
  size += kTlsHandshakesFull.size() + nchars(tlsHandshakesFull);
  size += kTlsHandshakesResumed.size() + nchars(tlsHandshakesResumed);
  size += kTlsHandshakesFailed.size() + nchars(tlsHandshakesFailed);
  size += kTlsHandshakesRejectedConcurrency.size() + nchars(tlsHandshakesRejectedConcurrency);
  size += kTlsHandshakesRejectedRateLimit.size() + nchars(tlsHandshakesRejectedRateLimit);
  size += kTlsClientCertPresent.size() + nchars(tlsClientCertPresent);
  size += kTlsAlpnStrictMismatches.size() + nchars(tlsAlpnStrictMismatches);

  size += MapSize(kTlsAlpnDistribution, kProtocolPrefix, tlsAlpnDistribution);
  size += MapSize(kTlsHandshakeFailureReasons, kReasonPrefix, tlsHandshakeFailureReasons);
  size += MapSize(kTlsVersionCounts, kVersionPrefix, tlsVersionCounts);
  size += MapSize(kTlsCipherCounts, kCipherPrefix, tlsCipherCounts);

  size += kTlsHandshakeDurationCount.size() + nchars(tlsHandshakeDurationCount);
  size += kTlsHandshakeDurationTotalNs.size() + nchars(tlsHandshakeDurationTotalNs);
  size += kTlsHandshakeDurationMaxNs.size() + nchars(tlsHandshakeDurationMaxNs);
#endif

  // ---- Second pass - single allocation, then direct writing  ------------
  std::string out;
  out.reserve(size);

  out.append(kOpen);
  out.append(kTotalBytesQueued);
  Append(out, totalBytesQueued);
  out.append(kTotalBytesWrittenImmediate);
  Append(out, totalBytesWrittenImmediate);
  out.append(kTotalBytesWrittenFlush);
  Append(out, totalBytesWrittenFlush);
  out.append(kDeferredWriteEvents);
  Append(out, deferredWriteEvents);
  out.append(kFlushCycles);
  Append(out, flushCycles);
  out.append(kEpollModFailures);
  Append(out, epollModFailures);
  out.append(kMaxConnectionOutboundBuffer);
  Append(out, maxConnectionOutboundBuffer);
  out.append(kTotalRequestsServed);
  Append(out, totalRequestsServed);

#ifdef AERONET_ENABLE_OPENSSL
  out.append(kKtlsSendEnabledConnections);
  Append(out, ktlsSendEnabledConnections);
  out.append(kKtlsSendEnableFallbacks);
  Append(out, ktlsSendEnableFallbacks);
  out.append(kKtlsSendForcedShutdowns);
  Append(out, ktlsSendForcedShutdowns);
  out.append(kKtlsSendBytes);
  Append(out, ktlsSendBytes);
  out.append(kTlsHandshakesSucceeded);
  Append(out, tlsHandshakesSucceeded);
  out.append(kTlsHandshakesFull);
  Append(out, tlsHandshakesFull);
  out.append(kTlsHandshakesResumed);
  Append(out, tlsHandshakesResumed);
  out.append(kTlsHandshakesFailed);
  Append(out, tlsHandshakesFailed);
  out.append(kTlsHandshakesRejectedConcurrency);
  Append(out, tlsHandshakesRejectedConcurrency);
  out.append(kTlsHandshakesRejectedRateLimit);
  Append(out, tlsHandshakesRejectedRateLimit);
  out.append(kTlsClientCertPresent);
  Append(out, tlsClientCertPresent);
  out.append(kTlsAlpnStrictMismatches);
  Append(out, tlsAlpnStrictMismatches);

  AppendMap(out, kTlsAlpnDistribution, kProtocolPrefix, tlsAlpnDistribution);
  AppendMap(out, kTlsHandshakeFailureReasons, kReasonPrefix, tlsHandshakeFailureReasons);
  AppendMap(out, kTlsVersionCounts, kVersionPrefix, tlsVersionCounts);
  AppendMap(out, kTlsCipherCounts, kCipherPrefix, tlsCipherCounts);

  out.append(kTlsHandshakeDurationCount);
  Append(out, tlsHandshakeDurationCount);
  out.append(kTlsHandshakeDurationTotalNs);
  Append(out, tlsHandshakeDurationTotalNs);
  out.append(kTlsHandshakeDurationMaxNs);
  Append(out, tlsHandshakeDurationMaxNs);
#endif

  out.append(kClose);

  assert(out.size() == size);
  return out;
}

}  // namespace aeronet