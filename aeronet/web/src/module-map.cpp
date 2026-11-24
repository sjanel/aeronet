#include "module-map.hpp"

#include <quickjs.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <string_view>

#include "aeronet/http-client.hpp"
#include "aeronet/memory-utils-sv.hpp"
#include "aeronet/raw-chars.hpp"

namespace aeronet::internal {

namespace {

// Copies 'str' (not a view of 'buffer') into 'buffer', null-terminated at its size, in one allocation at most.
std::string_view AssignTerminated(RawChars& buffer, std::string_view str) {
  buffer.clear();
  buffer.reserve(str.size() + 1U);
  *Append(str, buffer.data()) = '\0';
  buffer.setSize(str.size());
  return {buffer.data(), str.size()};
}

}  // namespace

ModuleMap::Compilation::Compilation(ModuleMap& map, std::string_view name) : _map(map) {
  const uint32_t index = map._compilations;
  map._compiling.emplace_back(name, index, index, static_cast<uint32_t>(map._pending.size()));
  ++map._compilations;
}

void ModuleMap::Compilation::loaded() {
  vector<Compiling>& compiling = _map._compiling;
  const Compiling& module = compiling.back();
  if (module.low == module.index) {
    // The first module of its cycles to complete: the modules pending since it started are in them, loaded too.
    _map._pending.erase(_map._pending.begin() + module.pendingSize, _map._pending.end());
    return;
  }
  // The module of index 'low' is compiling below its importer, which reaches it too.
  assert(compiling.size() >= 2U);
  _map._pending.emplace_back(RawChars32(module.name), module.low);
  Compiling& importer = compiling[compiling.size() - 2U];
  importer.low = std::min(importer.low, module.low);
}

ModuleMap::~ModuleMap() {
  JS_FreeValue(_pCtx, _graphError);
  for (const FailedModule& module : _failed) {
    JS_FreeValue(_pCtx, module.error);
  }
}

void ModuleMap::beginGraph(RequestCredentials credentials) noexcept {
  assert(_compiling.empty());
  JS_FreeValue(_pCtx, _graphError);
  _graphError = JS_UNDEFINED;
  _graphFetchFailed = false;
  _credentials = credentials;
  _compilations = 0;
  // Only left by a graph that a memory exhaustion of the C++ code ended.
  _pending.clear();
}

void ModuleMap::fail(JSValue error, bool fetchFailure) noexcept {
  ++_failures;
  if (!JS_IsUndefined(_graphError) && (_graphFetchFailed || !fetchFailure)) {
    JS_FreeValue(_pCtx, error);
    return;
  }
  JS_FreeValue(_pCtx, _graphError);
  _graphError = error;
  _graphFetchFailed = fetchFailure;
}

const ModuleMap::FailedModule* ModuleMap::failed(std::string_view name) const noexcept {
  const auto it = std::ranges::find_if(
      _failed, [name](const FailedModule& module) { return std::string_view(module.name) == name; });
  return it == _failed.end() ? nullptr : &*it;
}

void ModuleMap::addFailed(std::string_view name, JSValueConst error, bool fetchFailure) {
  // The entry first: its error is referenced once it cannot fail. An uncatchable error stops the scripts of its
  // deadline or memory exhaustion only.
  FailedModule& module = _failed.emplace_back(RawChars32(name), JS_UNDEFINED, false);
  if (!JS_IsUncatchableError(error)) {
    module.error = JS_DupValue(_pCtx, error);
    module.fetchFailure = fetchFailure;
  }
}

void ModuleMap::imported(std::string_view name) noexcept {
  if (_compiling.empty()) {
    return;
  }
  Compiling& importer = _compiling.back();
  const auto compilingIt = std::ranges::find(_compiling, name, &Compiling::name);
  if (compilingIt != _compiling.end()) {
    importer.low = std::min(importer.low, compilingIt->index);
    return;
  }
  const auto pendingIt = std::ranges::find_if(
      _pending, [name](const PendingModule& module) { return std::string_view(module.name) == name; });
  if (pendingIt != _pending.end()) {
    importer.low = std::min(importer.low, pendingIt->low);
  }
}

void ModuleMap::failPending() {
  // The module still compiling that a pending module reaches is below the failed one, or is it.
  for (const PendingModule& module : _pending) {
    addFailed(module.name, _graphError, _graphFetchFailed);
  }
  _pending.clear();
}

std::string_view ModuleMap::redirectedUrl(std::string_view name) const noexcept {
  const auto it = std::ranges::find_if(
      _redirected, [name](const RedirectedModule& module) { return std::string_view(module.name) == name; });
  return it == _redirected.end() ? std::string_view{} : std::string_view(it->url);
}

void ModuleMap::addRedirected(std::string_view name, std::string_view url) {
  _redirected.emplace_back(RawChars32(name), RawChars32(url));
}

const char* ModuleMap::terminated(std::string_view str) { return AssignTerminated(_terminated, str).data(); }

std::string_view ModuleMap::setCode(std::string_view code) { return AssignTerminated(_code, code); }

}  // namespace aeronet::internal
