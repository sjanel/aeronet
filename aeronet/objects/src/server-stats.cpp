
#include "aeronet/server-stats.hpp"

#include <string>
#include <string_view>

#include "aeronet/features.hpp"
#include "aeronet/stringconv.hpp"

namespace aeronet {

std::string ServerStats::json_str() const {
  std::string out;
  if constexpr (aeronet::openSslEnabled()) {
    out.reserve(512UL);
  } else {
    out.reserve(256UL);
  }
  out.push_back('{');
  out.append("\"totalBytesQueued\":").append(std::string_view(IntegralToCharVector(totalBytesQueued))).push_back(',');
  out.append("\"totalBytesWrittenImmediate\":")
      .append(std::string_view(IntegralToCharVector(totalBytesWrittenImmediate)))
      .push_back(',');
  out.append("\"totalBytesWrittenFlush\":")
      .append(std::string_view(IntegralToCharVector(totalBytesWrittenFlush)))
      .push_back(',');
  out.append("\"deferredWriteEvents\":")
      .append(std::string_view(IntegralToCharVector(deferredWriteEvents)))
      .push_back(',');
  out.append("\"flushCycles\":").append(std::string_view(IntegralToCharVector(flushCycles))).push_back(',');
  out.append("\"epollModFailures\":").append(std::string_view(IntegralToCharVector(epollModFailures))).push_back(',');
  out.append("\"maxConnectionOutboundBuffer\":")
      .append(std::string_view(IntegralToCharVector(maxConnectionOutboundBuffer)));
  out.push_back(',');
  out.append("\"totalRequestsServed\":").append(std::string_view(IntegralToCharVector(totalRequestsServed)));
#ifdef AERONET_ENABLE_OPENSSL
  out.append(",\"ktlsSendEnabledConnections\":")
      .append(std::string_view(IntegralToCharVector(ktlsSendEnabledConnections)));
  out.append(",\"ktlsSendEnableFallbacks\":").append(std::string_view(IntegralToCharVector(ktlsSendEnableFallbacks)));
  out.append(",\"ktlsSendForcedShutdowns\":").append(std::string_view(IntegralToCharVector(ktlsSendForcedShutdowns)));
  out.append(",\"ktlsSendBytes\":").append(std::string_view(IntegralToCharVector(ktlsSendBytes)));
  out.append(",\"tlsHandshakesSucceeded\":").append(std::string_view(IntegralToCharVector(tlsHandshakesSucceeded)));
  out.append(",\"tlsClientCertPresent\":").append(std::string_view(IntegralToCharVector(tlsClientCertPresent)));
  out.append(",\"tlsAlpnStrictMismatches\":").append(std::string_view(IntegralToCharVector(tlsAlpnStrictMismatches)));
  // ALPN distribution
  out.append(",\"tlsAlpnDistribution\":[");
  bool first = true;
  for (const auto& kv : tlsAlpnDistribution) {
    if (!first) {
      out.push_back(',');
    } else {
      first = false;
    }
    out.append(R"({"protocol":")")
        .append(kv.first)
        .append(R"(","count":)")
        .append(std::string_view(IntegralToCharVector(kv.second)))
        .append("}");
  }
  out.push_back(']');
  // TLS version counts
  out.append(",\"tlsVersionCounts\":[");
  first = true;
  for (const auto& kv : tlsVersionCounts) {
    if (!first) {
      out.push_back(',');
    } else {
      first = false;
    }
    out.append(R"({"version":")")
        .append(kv.first)
        .append(R"(","count":)")
        .append(std::string_view(IntegralToCharVector(kv.second)))
        .append("}");
  }
  out.push_back(']');
  // TLS cipher counts
  out.append(",\"tlsCipherCounts\":[");
  first = true;
  for (const auto& kv : tlsCipherCounts) {
    if (!first) {
      out.push_back(',');
    } else {
      first = false;
    }
    out.append(R"({"cipher":")")
        .append(kv.first)
        .append(R"(","count":)")
        .append(std::string_view(IntegralToCharVector(kv.second)))
        .append("}");
  }
  out.push_back(']');
  out.append(",\"tlsHandshakeDurationCount\":")
      .append(std::string_view(IntegralToCharVector(tlsHandshakeDurationCount)));
  out.append(",\"tlsHandshakeDurationTotalNs\":")
      .append(std::string_view(IntegralToCharVector(tlsHandshakeDurationTotalNs)));
  out.append(",\"tlsHandshakeDurationMaxNs\":")
      .append(std::string_view(IntegralToCharVector(tlsHandshakeDurationMaxNs)));
#endif
  out.push_back('}');
  return out;
}

}  // namespace aeronet
