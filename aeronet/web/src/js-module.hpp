#pragma once

#include <cstdint>
#include <string_view>

#include "aeronet/http-client.hpp"

namespace aeronet::internal {

class Page;

// How a module script ran (HTML "execute the script element").
enum class ModuleRun : uint8_t {
  Evaluated,    // its graph loaded and its evaluation started: its errors are reported as they come (top-level await)
  Failed,       // its graph (parse, link) or its evaluation failed with an error, reported
  FetchFailed,  // a module of its graph could not be fetched: an error event for its element, nothing reported
};

// Whether the pages run ES modules: only with a QuickJS-ng that frees them safely (see AERONET_QJS_MODULES in
// cmake/Dependencies.cmake). Without, the module scripts are skipped and the nomodule ones run, as in a browser without
// modules, and import() rejects.
[[nodiscard]] bool ModulesEnabled() noexcept;

// Makes 'page' load the module graphs of its runtime: the modules that its module scripts and import() reach are
// fetched with its HTTP client, in the cors mode, as subresources of its document, and compiled (or read back from the
// script cache of its session). Their requests count toward WebClientConfig::maxRequests (but those of the module
// scripts of the parser), their code toward WebClientConfig::maxScriptBytes.
void InstallModuleLoader(Page& page) noexcept;

// Runs the external module script of 'url' (absolute), its graph fetched with 'credentials' (its crossorigin
// attribute). Its own request is counted by the caller. Throws std::bad_alloc on memory exhaustion of the C++ code.
[[nodiscard]] ModuleRun RunExternalModule(Page& page, std::string_view url, RequestCredentials credentials);

// Runs the inline module script 'code' (null-terminated at its size, admitted by the caller in
// WebClientConfig::maxScriptBytes), its imports fetched with 'credentials'. Throws std::bad_alloc on memory exhaustion
// of the C++ code.
[[nodiscard]] ModuleRun RunInlineModule(Page& page, std::string_view code, RequestCredentials credentials);

}  // namespace aeronet::internal
