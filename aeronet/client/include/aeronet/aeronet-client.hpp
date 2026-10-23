// aeronet client Umbrella Header

#pragma once

#include "aeronet/http-client-config.hpp"     // IWYU pragma: export
#include "aeronet/http-client-error.hpp"      // IWYU pragma: export
#include "aeronet/http-client-exception.hpp"  // IWYU pragma: export
#include "aeronet/http-client.hpp"            // IWYU pragma: export
#include "aeronet/http-request.hpp"           // IWYU pragma: export
#include "aeronet/http-response.hpp"          // IWYU pragma: export
#include "aeronet/response-body-sink.hpp"     // IWYU pragma: export
#include "aeronet/retry-config.hpp"           // IWYU pragma: export
#include "aeronet/version.hpp"                // IWYU pragma: export

#ifdef AERONET_ENABLE_GLAZE
#include "aeronet/client-config-loader.hpp"      // IWYU pragma: export
#include "aeronet/http-client-config-glaze.hpp"  // IWYU pragma: export
#endif
