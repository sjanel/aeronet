// aeronet client Umbrella Header
//
// Glaze and protobuf are expensive to compile: include <aeronet/http-json.hpp> (JSON / YAML body helpers),
// <aeronet/http-client-config-glaze.hpp> (Glaze metadata of HttpClientConfig) and <aeronet/http-protobuf.hpp>
// (protobuf body helpers) in the files that use them. The configuration loader is included.

#pragma once

#include "aeronet/http-client-config.hpp"     // IWYU pragma: export
#include "aeronet/http-client-error.hpp"      // IWYU pragma: export
#include "aeronet/http-client-exception.hpp"  // IWYU pragma: export
#include "aeronet/http-client.hpp"            // IWYU pragma: export
#include "aeronet/http-request.hpp"           // IWYU pragma: export
#include "aeronet/http-response.hpp"          // IWYU pragma: export
#include "aeronet/response-body-sink.hpp"     // IWYU pragma: export
#include "aeronet/retry-config.hpp"           // IWYU pragma: export
#include "aeronet/upgraded-connection.hpp"    // IWYU pragma: export
#include "aeronet/version.hpp"                // IWYU pragma: export

#ifdef AERONET_ENABLE_WEBSOCKET
#include "aeronet/websocket-client.hpp"     // IWYU pragma: export
#include "aeronet/websocket-config.hpp"     // IWYU pragma: export
#include "aeronet/websocket-constants.hpp"  // IWYU pragma: export
#endif

#ifdef AERONET_ENABLE_GLAZE
#include "aeronet/client-config-loader.hpp"  // IWYU pragma: export
#endif
