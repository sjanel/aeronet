#pragma once

// Glaze metadata of the server configuration objects: include this header to read or write an HttpServerConfig, a
// RouterConfig or a StaticFileConfig with Glaze yourself.
//
// The glz::meta of a configuration type lives in a companion header named after it with a '-glaze' suffix (such as
// http-server-config-glaze.hpp), which includes the ones of its members, so that only the translation units that
// serialize configurations compile Glaze. Without it, serializing an HttpServerConfig, a RouterConfig or a TLSConfig
// does not compile, but a smaller object (CompressionConfig, AccessLogConfig...) would be written with its
// enumerations as integers and its internal members exposed.
#include "aeronet/http-server-config-glaze.hpp"  // IWYU pragma: export
#include "aeronet/router-config-glaze.hpp"       // IWYU pragma: export
#include "aeronet/server-config-loader.hpp"      // IWYU pragma: export
#include "aeronet/static-file-config-glaze.hpp"  // IWYU pragma: export
