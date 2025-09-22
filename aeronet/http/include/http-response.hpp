#pragma once

#include <string>

#include "http-status-code.hpp"

namespace aeronet {

struct HttpResponse {
  http::StatusCode statusCode{200};
  std::string reason{"OK"};
  std::string body{"Hello from aeronet"};
  std::string contentType{"text/plain"};
};

}  // namespace aeronet