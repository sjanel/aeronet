#pragma once

namespace aeronet {

class HttpRequest;
class HttpResponse;
struct HttpServerConfig;

namespace internal {
class ResponseCompressionState;

/// Applies server-side response adjustments prior to sending:
/// - increments per-connection and global request counters (if provided)
/// - injects default 404 body when missing (for non-HEAD)
/// - applies in-memory response compression based on Accept-Encoding (for non-HEAD)
void PrefinalizeHttpResponse(const HttpRequest& request, HttpResponse& response, bool isHead,
                             ResponseCompressionState& compressionState, const HttpServerConfig& serverConfig);

}  // namespace internal
}  // namespace aeronet
