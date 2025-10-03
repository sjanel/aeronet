// Aeronet Umbrella Header
//
// Include this single header to pull in the core public HTTP server API:
//   - HttpServer / AsyncHttpServer / MultiHttpServer wrappers
//   - Configuration types (HttpServerConfig, CompressionConfig, Encoding)
//   - Request / Response primitives (HttpRequest, HttpResponse, HttpResponseWriter)
//   - HTTP enums & helpers (methods, method sets, status codes, version)
//   - Server statistics structure
//
// Design Notes:
//  * This header intentionally avoids exposing lower‑level implementation details that are
//    considered internal (e.g. ConnectionState internals, parser utilities). Public types
//    needed by user code for handler logic and configuration are re‑exported.
//  * Optional features (compression, TLS) are conditionally included based on build flags to
//    avoid forcing downstream translation units to parse unused heavy headers.
//  * To keep Include‑What‑You‑Use (IWYU) / clang-tidy include-cleaner happy when users only
//    include <aeronet/aeronet.hpp>, each re-exported header line is annotated with
//      IWYU pragma: export
//    so that symbols they provide are treated as satisfied for direct use in user code.
//  * If you prefer more granular control (to minimize compile time), you can include the
//    specific individual headers instead of this umbrella.
//
// Stability: Pre-1.0 API — surface may evolve; this umbrella will track new top-level
// components but aims to remain a convenient superset for typical server usage.
//
// Usage Example:
//    #include <aeronet/aeronet.hpp>
//    using namespace aeronet;
//    int main() {
//      HttpServer server(HttpServerConfig{}.withPort(0));
//      server.setHandler([](const HttpRequest& req){
//         return HttpResponse(200, "OK").contentType("text/plain").body("hi\n");
//      });
//      server.run();
//    }
//
// NOTE: Keep this header lightweight: do not add large STL or system headers here; rely on
// transitive includes from the specific components they belong to. This keeps build times
// reasonable when users adopt the umbrella.

#pragma once

// Core server & wrappers
#include "aeronet/async-http-server.hpp"  // IWYU pragma: export
#include "aeronet/http-server.hpp"        // IWYU pragma: export
#include "aeronet/multi-http-server.hpp"  // IWYU pragma: export

// Configuration
#include "aeronet/compression-config.hpp"  // IWYU pragma: export
#include "aeronet/encoding.hpp"            // IWYU pragma: export
#include "aeronet/http-server-config.hpp"  // IWYU pragma: export

// HTTP primitives
#include "aeronet/http-request.hpp"          // IWYU pragma: export
#include "aeronet/http-response-writer.hpp"  // IWYU pragma: export
#include "aeronet/http-response.hpp"         // IWYU pragma: export

// HTTP protocol enums & helpers
#include "aeronet/http-constants.hpp"    // IWYU pragma: export
#include "aeronet/http-method-set.hpp"   // IWYU pragma: export
#include "aeronet/http-method.hpp"       // IWYU pragma: export
#include "aeronet/http-status-code.hpp"  // IWYU pragma: export
#include "aeronet/http-version.hpp"      // IWYU pragma: export

// Stats / metrics surface
#include "aeronet/server-stats.hpp"  // IWYU pragma: export

// End of aeronet.hpp