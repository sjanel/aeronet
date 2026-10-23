#pragma once

#include <glaze/glaze.hpp>

#include "aeronet/access-log-config.hpp"

template <>
struct glz::meta<aeronet::AccessLogConfig::Format> {
  using enum aeronet::AccessLogConfig::Format;
  static constexpr auto value = enumerate("clf", CLF, "json", JSON);
};

template <>
struct glz::meta<aeronet::AccessLogConfig::Sink> {
  using enum aeronet::AccessLogConfig::Sink;
  static constexpr auto value = enumerate("none", None, "stdout", Stdout, "file", File);
};
