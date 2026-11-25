#pragma once

#include <quickjs.h>

#include <cstdint>
#include <string_view>
#include <type_traits>

#include "aeronet/amc-type-traits.hpp"
#include "aeronet/http-client.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/vector.hpp"

namespace aeronet::internal {

// The state of the ES modules of a page beside what QuickJS-ng keeps (its loaded modules, by name: their URL): the
// modules that failed to load with the error they rethrow, the URL of the redirected ones, and the module graph being
// loaded (see js-module.cpp).
//
// A graph loads synchronously: QuickJS-ng resolves the imports of a module when it compiles it, through the hooks of
// js-module.cpp, which fetch and compile each import in turn, nested. Within a resolution, the hooks never fail:
// QuickJS-ng does not undo a failed one (a module that imported the failing module through a cycle keeps a pointer to
// it once freed, and a module whose resolution started stays resolved without its next imports), so a later link of the
// graph would read freed memory. A failure is recorded in the graph instead, the import resolved to a module that
// throws (kFailedModuleName), and the graph is never evaluated: its root fails, where failing is safe. Only a memory
// exhaustion inside QuickJS-ng can still fail a resolution: the map is then broken, and no module loads any more.
//
// QuickJS-ng keeps the modules of a failed graph loaded, and a later import of one of them would reuse it: each module
// that reaches a failure is recorded as failed, so that its next imports fail without running anything. A module
// reaches the failures of the imports of its compilation, and, through a cycle, those of the modules still compiling
// that it imports: the compilations walk the graph depth first, and its cycles are found as by Tarjan's algorithm.
class ModuleMap {
 public:
  // The name of the module that a failed import resolves to: never evaluated, as its graph fails.
  static constexpr char kFailedModuleName[] = "aeronet:failed-module";

  // A module that failed to load (fetch, parse, one of its imports, a module that it reaches), named by its URL.
  struct FailedModule {
    using trivially_relocatable = amc::is_trivially_relocatable<RawChars32>::type;

    // Whether an uncatchable error (the deadline, a memory exhaustion) stopped its graph after its compilation: its
    // next imports fail with a TypeError instead, as the scripts that catch errors cannot catch that one.
    [[nodiscard]] bool stopped() const noexcept { return JS_IsUndefined(error); }

    RawChars32 name;
    JSValue error;      // rethrown by the next imports, undefined when stopped()
    bool fetchFailure;  // a fetch failure (an error event for a module script), else an error to report
  };

  // The compilation of the module 'name' (a view valid for its lifetime) in the graph being loaded: the hooks run
  // nested within, as its imports resolve. Not loaded() means failed.
  class Compilation {
   public:
    // Throws std::bad_alloc on memory exhaustion.
    Compilation(ModuleMap& map, std::string_view name);

    Compilation(const Compilation&) = delete;
    Compilation& operator=(const Compilation&) = delete;
    Compilation(Compilation&&) = delete;
    Compilation& operator=(Compilation&&) = delete;

    ~Compilation() { _map._compiling.pop_back(); }

    // The module compiled and loaded without failure in its graph. Throws std::bad_alloc on memory exhaustion.
    void loaded();

   private:
    ModuleMap& _map;
  };

  // The load of the graph of a module script of the document (whose root request the page counts), its modules fetched
  // with 'credentials': the next root is that of an import().
  class ScriptGraph {
   public:
    ScriptGraph(ModuleMap& map, RequestCredentials credentials) noexcept : _map(map) {
      map.beginGraph(credentials);
      map._topLevel = true;
    }

    ScriptGraph(const ScriptGraph&) = delete;
    ScriptGraph& operator=(const ScriptGraph&) = delete;
    ScriptGraph(ScriptGraph&&) = delete;
    ScriptGraph& operator=(ScriptGraph&&) = delete;

    ~ScriptGraph() { _map._topLevel = false; }

   private:
    ModuleMap& _map;
  };

  explicit ModuleMap(JSContext* pCtx) noexcept : _pCtx(pCtx) {}

  ModuleMap(const ModuleMap&) = delete;
  ModuleMap& operator=(const ModuleMap&) = delete;
  ModuleMap(ModuleMap&&) = delete;
  ModuleMap& operator=(ModuleMap&&) = delete;

  ~ModuleMap();

  // Whether the hooks run at the root of a graph (an import(), a module script), where they can fail.
  [[nodiscard]] bool atRoot() const noexcept { return _compiling.empty(); }

  // Whether a resolution failed inside QuickJS-ng (memory exhaustion): no module loads any more.
  [[nodiscard]] bool broken() const noexcept { return _broken; }
  void setBroken() noexcept { _broken = true; }

  // Counts a call of a hook: a compilation during which the count changed started a resolution.
  void countHookCall() noexcept { ++_hookCalls; }
  [[nodiscard]] uint32_t hookCalls() const noexcept { return _hookCalls; }

  // Starts the load of the graph of an import(), whose modules are fetched with 'credentials' (see ScriptGraph for a
  // module script). Forgets the failures of the previous graph.
  void beginGraph(RequestCredentials credentials) noexcept;

  // Whether the graph is that of a module script (see ScriptGraph).
  [[nodiscard]] bool topLevel() const noexcept { return _topLevel; }

  [[nodiscard]] RequestCredentials credentials() const noexcept { return _credentials; }

  // Records a failure of the graph, its 'error' owned: a fetch failure wins over the other errors, else the first one
  // is kept.
  void fail(JSValue error, bool fetchFailure) noexcept;

  // The number of failures recorded: a module whose imports added some failed.
  [[nodiscard]] uint32_t failures() const noexcept { return _failures; }

  // The error of the graph (see fail()), undefined without failure.
  [[nodiscard]] JSValueConst graphError() const noexcept { return _graphError; }

  [[nodiscard]] bool graphFetchFailed() const noexcept { return _graphFetchFailed; }

  // The failed module named 'name', null for none.
  [[nodiscard]] const FailedModule* failed(std::string_view name) const noexcept;

  // Records that the module 'name' failed with 'error' (a new reference is kept, unless it is uncatchable: see
  // FailedModule::stopped()).
  void addFailed(std::string_view name, JSValueConst error, bool fetchFailure);

  // The module being compiled imports the module 'name' (loaded, compiling, or about to load): through a cycle, it
  // reaches what that one reaches. Nothing at the root of a graph.
  void imported(std::string_view name) noexcept;

  // A module failed in the graph: the modules that reach a module still compiling fail with the error of the graph, as
  // that module reaches the failed one (its imports lead to it).
  void failPending();

  // The URL of the referrer of the request of the module about to load (see NormalizeModuleName()): a view valid
  // until the loader returns.
  [[nodiscard]] std::string_view referrer() const noexcept { return _referrer; }
  void setReferrer(std::string_view url) noexcept { _referrer = url; }

  // The URL of the response of the module named 'name' when a redirect led to it, its base URL, empty otherwise.
  [[nodiscard]] std::string_view redirectedUrl(std::string_view name) const noexcept;

  void addRedirected(std::string_view name, std::string_view url);

  // 'str' null-terminated in a buffer of the map, valid until the next call.
  [[nodiscard]] const char* terminated(std::string_view str);

  // 'code', the code of a module about to compile, null-terminated at its size in a buffer of the map. Reused by the
  // nested ones: QuickJS-ng parses the whole code before it resolves the imports.
  [[nodiscard]] std::string_view setCode(std::string_view code);

 private:
  struct RedirectedModule {
    using trivially_relocatable = amc::is_trivially_relocatable<RawChars32>::type;

    RawChars32 name;
    RawChars32 url;
  };

  // A module being compiled: the compilations from the root of the graph form a path, the imports being compiled.
  struct Compiling {
    std::string_view name;
    uint32_t index;        // the order of its compilation in the graph
    uint32_t low;          // the lowest index of a module still compiling that it reaches, its own without cycle
    uint32_t pendingSize;  // the size of _pending when it started
  };

  // A module loaded without failure that reaches a module still compiling, of index 'low': it fails if that one fails.
  // The first module of their cycles to complete loads them all.
  struct PendingModule {
    using trivially_relocatable = amc::is_trivially_relocatable<RawChars32>::type;

    RawChars32 name;
    uint32_t low;
  };

  vector<FailedModule> _failed;
  vector<RedirectedModule> _redirected;
  vector<Compiling> _compiling;
  vector<PendingModule> _pending;
  RawChars _code;
  RawChars _terminated;
  JSValue _graphError{JS_UNDEFINED};
  std::string_view _referrer;
  JSContext* _pCtx;
  uint32_t _compilations{};  // of the graph
  uint32_t _hookCalls{};
  uint32_t _failures{};
  RequestCredentials _credentials{RequestCredentials::SameOrigin};
  bool _graphFetchFailed{};
  bool _topLevel{};
  bool _broken{};
};

}  // namespace aeronet::internal
