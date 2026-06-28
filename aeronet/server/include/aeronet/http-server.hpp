#pragma once

#include "aeronet/multi-http-server.hpp"

namespace aeronet {

// First class Server object to be used in user code.
// Synonym of MultiHttpServer.
using HttpServer = MultiHttpServer;

}  // namespace aeronet