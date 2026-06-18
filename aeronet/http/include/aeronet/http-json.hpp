#pragma once

// Opt-in JSON / YAML body helpers for HttpRequest and HttpResponse.
//
// These pull in the (heavy, compile-time expensive) Glaze dependency. To keep that cost out of the
// widely-included core headers (<aeronet/http-request.hpp> / <aeronet/http-response.hpp>), the
// member function templates declared there are *defined* here instead. Translation units that
// actually serialize/parse JSON or YAML include this header explicitly; everything else stays cheap.
//
// The public API is unchanged: include this header and keep calling
//   response.bodyJson(obj) / response.bodyYaml(obj)
//   request.bodyAs<T>()    / request.bodyAsYaml<T>()

#ifdef AERONET_ENABLE_GLAZE

#include <expected>
#include <glaze/glaze.hpp>
#include <glaze/yaml.hpp>  // IWYU pragma: keep
#include <stdexcept>
#include <string>
#include <utility>

#include "aeronet/http-constants.hpp"
#include "aeronet/http-request.hpp"  // IWYU pragma: keep (also brings http-response.hpp)
#include "aeronet/http-response.hpp"
#include "aeronet/http-status-code.hpp"

namespace aeronet {

template <class T>
HttpResponse& HttpResponse::bodyJson(const T& obj) & {
  std::string buf;
  if (const auto ec = glz::write<glz::opts{}>(obj, buf)) [[unlikely]] {
    throw std::runtime_error("bodyJson serialization failed: " + glz::format_error(ec));
  }
  return body(std::move(buf), http::ContentTypeApplicationJson);
}

template <class T>
HttpResponse& HttpResponse::bodyYaml(const T& obj) & {
  std::string buf;
  if (const auto ec = glz::write<glz::opts{.format = glz::YAML}>(obj, buf)) [[unlikely]] {
    throw std::runtime_error("bodyYaml serialization failed: " + glz::format_error(ec));
  }
  return body(std::move(buf), "text/yaml");
}

template <class T>
std::expected<T, HttpResponse> HttpRequest::bodyAs() const {
  T obj{};
  // body() views the receive buffer and is NOT null-terminated, so glaze must honour the end
  // pointer strictly (its default assumes a '\0' sentinel and would read past the body).
  if (const auto ec = glz::read<glz::opts{.null_terminated = false}>(obj, body())) [[unlikely]] {
    return std::unexpected(
        makeResponse(http::StatusCodeBadRequest, glz::format_error(ec, body()), http::ContentTypeTextPlain));
  }
  return obj;
}

template <class T>
std::expected<T, HttpResponse> HttpRequest::bodyAsYaml() const {
  T obj{};
  if (const auto ec = glz::read<glz::opts{.format = glz::YAML, .null_terminated = false}>(obj, body())) [[unlikely]] {
    return std::unexpected(
        makeResponse(http::StatusCodeBadRequest, glz::format_error(ec, body()), http::ContentTypeTextPlain));
  }
  return obj;
}

}  // namespace aeronet

#endif  // AERONET_ENABLE_GLAZE
