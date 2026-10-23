#pragma once

#include <glaze/glaze.hpp>

#include "aeronet/client-protocol.hpp"

template <>
struct glz::meta<aeronet::HttpVersionMode> {
  using enum aeronet::HttpVersionMode;
#ifdef AERONET_ENABLE_HTTP2
  static constexpr auto value = enumerate("auto", Auto, "http1.1", Http1_1, "http2", Http2);
#else
  static constexpr auto value = enumerate("auto", Auto, "http1.1", Http1_1);
#endif
};
