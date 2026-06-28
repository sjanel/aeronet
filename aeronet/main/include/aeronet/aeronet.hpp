// aeronet Umbrella Header
//
// Include this single header to pull in the core public HTTP server API:
//   - Server types (SingleHttpServer / MultiHttpServer)
//   - Configuration types (HttpServerConfig, CompressionConfig, Encoding, etc)
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

#pragma once

#include "aeronet/aeronet-server.hpp"  // IWYU pragma: export

#ifdef AERONET_ENABLE_HTTP_CLIENT
#include "aeronet/aeronet-client.hpp"  // IWYU pragma: export
#endif