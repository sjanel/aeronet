#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "aeronet/http-client.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/vector.hpp"

struct lxb_dom_node;

namespace aeronet::internal {

// Whether 'type' is a JavaScript MIME type essence (MIME Sniffing: "text/javascript", "application/javascript"...),
// in any case: the type of a script, and the Content-Type of a module script without its parameters.
[[nodiscard]] bool IsJavaScriptMimeType(std::string_view type) noexcept;

// The scripts of a document in the order of a browser (HTML "prepare the script element"): the parser-blocking classic
// ones, with those that document.write() inserts after the running one, then the defer ones (classic or module), then
// the async ones (in document order: their fetches are immediate), and apart, the scripts that scripts insert to run as
// tasks (external classic scripts and module scripts), in the order of their insertion. It also tracks the script
// elements that scripts create (runnable once connected), holds the running script, the parser script whose
// document.write() output it keeps, and the bytes of the scripts run.
class ScriptQueue {
 public:
  // The scripts of the parser.
  enum class Kind : uint8_t { Blocking, Defer, Async };

  // How a script element created by a script starts once connected (see start()).
  enum class Start : uint8_t { No, Inline, External, Module };

  // What a script inserted by a script runs as a task.
  enum class Inserted : uint8_t { Classic, ExternalModule, InlineModule };

  // A script of the parser, of its type when it was prepared (a later change of its type attribute changes nothing).
  struct ParserScript {
    lxb_dom_node* pScript;  // null for none
    bool module;            // a module script, else a classic one
  };

  // A script that a script inserted, to run as a task, as its preparation read it (HTML "prepare the script
  // element"): a later change of its attributes or of its text does not change what runs.
  struct InsertedScript {
    std::string_view url;            // resolved against the base URL, empty when its src is empty or not a valid URL
    std::string_view text;           // the charset attribute of a classic script, the code of an inline module
    lxb_dom_node* pScript;           // null for none
    RequestCredentials credentials;  // of its crossorigin attribute
    Inserted kind;
  };

  // Adds the scripts among the siblings from pFirst to pEnd (excluded, null for after the last one) and their
  // descendants: the blocking ones run right after the current one. 'url', of the document, is for the logs.
  void collect(lxb_dom_node* pFirst, lxb_dom_node* pEnd, std::string_view url);

  // Notes pElement, an element created by a script (createElement(), createElementNS()): a script element runs once
  // connected, unlike those of the parser (already queued) and of the HTML fragments (innerHTML), which never run.
  void elementCreated(lxb_dom_node* pElement);

  // Notes pCopy, a copy of pSource made by cloneNode() (called for each node of a deep copy): the copy of a script
  // created by a script and not started is runnable too (HTML "cloning steps" of a script, which copy its "already
  // started").
  void cloned(const lxb_dom_node* pSource, lxb_dom_node* pCopy) {
    if (isCreated(pSource)) {
      _created.push_back(pCopy);
    }
  }

  // Whether a script element created by a script has not started yet: an insertion checks for them only then.
  [[nodiscard]] bool hasCreated() const noexcept { return !_created.empty(); }

  // Whether pNode is a script element created by a script, not started yet.
  [[nodiscard]] bool isCreated(const lxb_dom_node* pNode) const noexcept;

  // Whether a script element created by a script and not started yet is the parent of the siblings from pFirst to pEnd
  // (excluded, null for after the last one), one of them or one of their descendants. In O(number of such scripts x
  // their depth), whatever the number of inserted nodes.
  [[nodiscard]] bool hasCreatedIn(const lxb_dom_node* pFirst, const lxb_dom_node* pEnd) const noexcept;

  // Starts pScript, a connected script element, if a script created it and it has not started (HTML "prepare the
  // script element" of a script that is not parser-inserted): Inline when it is a classic script with no src but some
  // text, to run now, External when it is a classic script with a src, and Module for a module script with a src or
  // some text, for the caller to queue with queueInserted(), No when it does not start (an empty one waits for a src or
  // text, as does a script of another type). 'url', of the document, is for the logs.
  [[nodiscard]] Start start(lxb_dom_node* pScript, std::string_view url);

  // Queues 'script', which start() found External or Module, whose views are copied.
  void queueInserted(const InsertedScript& script);

  // The next script inserted by a script to run as a task, taken from the queue, with a null pScript when none is
  // left. Its views are valid until the next queueInserted().
  [[nodiscard]] InsertedScript nextInserted() noexcept;

  // The next script of the parser of 'kind', taken from the queue, with a null pScript when none is left.
  [[nodiscard]] ParserScript next(Kind kind) noexcept {
    const auto index = static_cast<std::size_t>(kind);
    return _next[index] < _scripts[index].size() ? _scripts[index][_next[index]++] : ParserScript{nullptr, false};
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

  // The running script, null outside of one (document.currentScript).
  [[nodiscard]] lxb_dom_node* current() const noexcept { return _pCurrent; }
  void setCurrent(lxb_dom_node* pScript) noexcept { _pCurrent = pScript; }

  // The running parser script, null outside of one: the scripts that it inserts and run at once (inline) write after
  // it too, the others cannot write (HTML "ignore-destructive-writes counter").
  [[nodiscard]] lxb_dom_node* writer() const noexcept { return _pWriter; }
  void setWriter(lxb_dom_node* pScript) noexcept { _pWriter = pScript; }

  // The output of the document.write() calls of the writer, inserted after it when it ends.
  [[nodiscard]] RawChars& written() noexcept { return _written; }

 private:
  // An entry of the queue of the inserted scripts, whose URL then text are next in _insertedText.
  struct QueuedScript {
    lxb_dom_node* pScript;
    std::size_t urlSize;
    std::size_t textSize;
    RequestCredentials credentials;
    Inserted kind;
  };

  static constexpr std::size_t kNbKinds = 3;

  vector<ParserScript> _scripts[kNbKinds];
  vector<lxb_dom_node*> _created;  // the script elements created by scripts, not started yet
  vector<QueuedScript> _inserted;
  RawChars _insertedText;  // the URL and the text of each entry of _inserted, back to back
  RawChars _written;
  std::size_t _bytesRun{};
  std::size_t _insertedTextPos{};  // of the next entry of _inserted
  lxb_dom_node* _pCurrent{};
  lxb_dom_node* _pWriter{};
  vector<ParserScript>::size_type _next[kNbKinds]{};
  vector<QueuedScript>::size_type _nextInserted{};
};

}  // namespace aeronet::internal
