#pragma once

#include <filesystem>
#include <string_view>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/static-file-config.hpp"

namespace aeronet {

// Serves files from a fixed root directory with RFC 7233 / RFC 7232 semantics.
// Can be used as a RequestHandler callable handler in Router.
class StaticFileHandler {
 public:
  explicit StaticFileHandler(std::filesystem::path rootDirectory, StaticFileConfig config = {});

  /// Build a response for the given request. Only GET and HEAD are served.
  [[nodiscard]] HttpResponse operator()(const HttpRequest& request) const;

 private:
  [[nodiscard]] bool resolveTarget(const HttpRequest& request, std::filesystem::path& resolvedPath) const;

  [[nodiscard]] static HttpResponse makeError(http::StatusCode code, std::string_view reason);

  std::filesystem::path _root;
  StaticFileConfig _config;
};

}  // namespace aeronet
