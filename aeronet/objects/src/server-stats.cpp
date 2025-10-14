
#include "aeronet/server-stats.hpp"

#include <string>

namespace aeronet {

std::string ServerStats::json_str() const {
  std::string out;
#ifdef AERONET_ENABLE_OPENSSL
  out.reserve(512UL);
#else
  out.reserve(256UL);
#endif
  out.push_back('{');
  out.append("\"totalBytesQueued\":").append(std::to_string(totalBytesQueued)).push_back(',');
  out.append("\"totalBytesWrittenImmediate\":").append(std::to_string(totalBytesWrittenImmediate)).push_back(',');
  out.append("\"totalBytesWrittenFlush\":").append(std::to_string(totalBytesWrittenFlush)).push_back(',');
  out.append("\"deferredWriteEvents\":").append(std::to_string(deferredWriteEvents)).push_back(',');
  out.append("\"flushCycles\":").append(std::to_string(flushCycles)).push_back(',');
  out.append("\"epollModFailures\":").append(std::to_string(epollModFailures)).push_back(',');
  out.append("\"maxConnectionOutboundBuffer\":").append(std::to_string(maxConnectionOutboundBuffer));
#ifdef AERONET_ENABLE_OPENSSL
  out.append(",\"tlsHandshakesSucceeded\":").append(std::to_string(tlsHandshakesSucceeded));
  out.append(",\"tlsClientCertPresent\":").append(std::to_string(tlsClientCertPresent));
  out.append(",\"tlsAlpnStrictMismatches\":").append(std::to_string(tlsAlpnStrictMismatches));
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
        .append(std::to_string(kv.second))
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
        .append(std::to_string(kv.second))
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
    out.append(R"({"cipher":")").append(kv.first).append(R"(","count":)").append(std::to_string(kv.second)).append("}");
  }
  out.push_back(']');
  out.append(",\"tlsHandshakeDurationCount\":").append(std::to_string(tlsHandshakeDurationCount));
  out.append(",\"tlsHandshakeDurationTotalNs\":").append(std::to_string(tlsHandshakeDurationTotalNs));
  out.append(",\"tlsHandshakeDurationMaxNs\":").append(std::to_string(tlsHandshakeDurationMaxNs));
#endif
  out.push_back('}');
  return out;
}

}  // namespace aeronet
