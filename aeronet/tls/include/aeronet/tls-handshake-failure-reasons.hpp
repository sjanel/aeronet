#pragma once

#include <string_view>

namespace aeronet {

inline constexpr std::string_view kTlsHandshakeFailureReasonAlpnStrictMismatch = "alpn_strict_mismatch";
inline constexpr std::string_view kTlsHandshakeFailureReasonEof = "handshake_eof";
inline constexpr std::string_view kTlsHandshakeFailureReasonError = "handshake_error";
inline constexpr std::string_view kTlsHandshakeFailureReasonHandshakeTimeout = "handshake_timeout";
inline constexpr std::string_view kTlsHandshakeFailureReasonRejectedConcurrency = "rejected_concurrency";
inline constexpr std::string_view kTlsHandshakeFailureReasonRejectedRateLimit = "rejected_rate_limit";
inline constexpr std::string_view kTlsHandshakeFailureReasonSslNewFailed = "ssl_new_failed";
inline constexpr std::string_view kTlsHandshakeFailureReasonSslSetFdFailed = "ssl_set_fd_failed";
inline constexpr std::string_view kTlsHandshakeFailureReasonSetExDataFailed = "ssl_set_ex_data_failed";

}  // namespace aeronet