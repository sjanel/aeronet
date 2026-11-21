#pragma once

#include <quickjs.h>

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "aeronet/amc-type-traits.hpp"
#include "aeronet/http-client.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/vector.hpp"
#include "cors.hpp"
#include "memory-budget.hpp"

namespace aeronet::internal {

// The requests of the scripts of a page (fetch(), XMLHttpRequest, navigator.sendBeacon()) not sent yet: the page loop
// sends them one at a time, as tasks between the others, in the order the scripts made them. Each holds the
// JavaScript callback of its response, freed with it. Their native memory is counted in the budget of the page.
class RequestQueue {
 public:
  // A request of a script, ready to send.
  struct Request {
    using trivially_relocatable = std::bool_constant<amc::is_trivially_relocatable_v<HttpRequest> &&
                                                     amc::is_trivially_relocatable_v<RawChars32>>::type;

    HttpRequest http;        // with the headers of the script, and its Accept, Origin and Referer
    RawChars32 corsHeaders;  // the Access-Control-Request-Headers of its preflight: sorted names separated by ','
    JSValue callback;        // called with the response, owned by the queue until taken
    std::size_t memory;      // counted in the budget of the page while queued
    RequestMode mode;
    RequestCredentials credentials;
    bool crossOrigin;  // its URL, or one of its redirects, is not of the origin of the document
    bool preflight;    // it needs a CORS preflight at its current URL
  };

  RequestQueue(JSContext* pCtx, MemoryBudget& memory) noexcept : _pCtx(pCtx), _pMemory(&memory) {}

  RequestQueue(const RequestQueue&) = delete;
  RequestQueue& operator=(const RequestQueue&) = delete;
  RequestQueue(RequestQueue&&) = delete;
  RequestQueue& operator=(RequestQueue&&) = delete;

  ~RequestQueue();

  // Counts a request about to be made: false, nothing counted, when the document already made 'maxRequests'.
  [[nodiscard]] bool admit(uint32_t maxRequests) noexcept {
    if (_made >= maxRequests) {
      return false;
    }
    ++_made;
    return true;
  }

  // Queues 'request', whose callback is 'callback' (a new reference is kept) and whose memory is request.memory.
  // Returns its id (positive), 0 when the page cannot hold it (nothing queued).
  [[nodiscard]] int32_t add(Request request, JSValueConst callback);

  // Removes the request 'id' if not sent yet.
  void cancel(int32_t id) noexcept;

  [[nodiscard]] bool empty() const noexcept { return _pending.empty(); }

  // Takes the oldest request, whose callback the caller then owns. The queue must not be empty.
  [[nodiscard]] Request takeNext() noexcept;

 private:
  struct Pending {
    using trivially_relocatable = amc::is_trivially_relocatable<Request>::type;

    Request request;
    int32_t id;
  };

  vector<Pending> _pending;
  JSContext* _pCtx;
  MemoryBudget* _pMemory;
  uint32_t _made{};
  int32_t _nextId{1};
};

}  // namespace aeronet::internal
