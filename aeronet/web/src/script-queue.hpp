#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "aeronet/raw-chars.hpp"
#include "aeronet/vector.hpp"

struct lxb_dom_node;

namespace aeronet::internal {

// The classic scripts of a document in the order of a browser (HTML "prepare the script element"): the parser-blocking
// ones, with those that document.write() inserts after the running one, then the defer ones, then the async ones (in
// document order: their fetches are immediate). It also holds the running script, with the output of its
// document.write() calls, and the bytes of the scripts run.
class ScriptQueue {
 public:
  enum class Kind : uint8_t { Blocking, Defer, Async };

  // Adds the classic scripts among the siblings from pFirst to pEnd (excluded, null for after the last one) and their
  // descendants: the blocking ones run right after the current one. 'url', of the document, is for the logs.
  void collect(lxb_dom_node* pFirst, lxb_dom_node* pEnd, std::string_view url);

  // The next script of 'kind', taken from the queue, null when none is left.
  [[nodiscard]] lxb_dom_node* next(Kind kind) noexcept {
    const auto index = static_cast<std::size_t>(kind);
    return _next[index] < _scripts[index].size() ? _scripts[index][_next[index]++] : nullptr;
  }

  // Counts the 'size' bytes of the code of a script about to run: false, and nothing counted, when they would exceed
  // 'maxBytes' with those of the scripts run before.
  [[nodiscard]] bool admit(std::size_t size, std::size_t maxBytes) noexcept {
    if (size > maxBytes - _bytesRun) {
      return false;
    }
    _bytesRun += size;
    return true;
  }

  // The script run by the page loop, null outside of one (document.currentScript, document.write()).
  [[nodiscard]] lxb_dom_node* current() const noexcept { return _pCurrent; }
  void setCurrent(lxb_dom_node* pScript) noexcept { _pCurrent = pScript; }

  // The output of the document.write() calls of the current script, inserted after it when it ends.
  [[nodiscard]] RawChars& written() noexcept { return _written; }

 private:
  using Scripts = vector<lxb_dom_node*>;

  static constexpr std::size_t kNbKinds = 3;

  Scripts _scripts[kNbKinds];
  RawChars _written;
  std::size_t _bytesRun{};
  lxb_dom_node* _pCurrent{};
  Scripts::size_type _next[kNbKinds]{};
};

}  // namespace aeronet::internal
