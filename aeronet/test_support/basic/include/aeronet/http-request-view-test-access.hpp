#pragma once

#include "aeronet/http-request-view.hpp"

namespace aeronet::test {

// Creates requests for the tests of the code taking an HttpRequestView, outside of a server (its constructor is
// private).
struct HttpRequestViewTestAccess {
  [[nodiscard]] static HttpRequestView Make() { return {}; }
};

}  // namespace aeronet::test
