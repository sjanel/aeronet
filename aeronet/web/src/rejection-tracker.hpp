#pragma once

#include <quickjs.h>

#include <string_view>

#include "aeronet/vector.hpp"

namespace aeronet::internal {

// The rejected promises without handler of a page (HTML "notify about rejected promises"): reported when the task
// ends, unless a handler was added meanwhile. They hold JavaScript values of the context, freed with them.
class RejectionTracker {
 public:
  // Called with the text of a reason ("Uncaught (in promise) ..."), can throw.
  using Report = void (*)(void* pContext, std::string_view text);

  explicit RejectionTracker(JSContext* pCtx) noexcept : _pCtx(pCtx) {}

  RejectionTracker(const RejectionTracker&) = delete;
  RejectionTracker& operator=(const RejectionTracker&) = delete;
  RejectionTracker(RejectionTracker&&) = delete;
  RejectionTracker& operator=(RejectionTracker&&) = delete;

  ~RejectionTracker();

  // 'promise' was rejected without handler ('isHandled' false), or got its first handler after its rejection (true).
  // Called by QuickJS, it cannot throw: a rejection it cannot track for lack of memory makes flush() throw.
  void track(JSValueConst promise, JSValueConst reason, bool isHandled) noexcept;

  // Gives the reason of each tracked rejection to report(pContext, text), forgotten first: formatting a reason can run
  // scripts (toString()) that reject promises, reported by the next flush. Then throws std::bad_alloc if a rejection
  // could not be tracked since the previous flush.
  void flush(Report report, void* pContext);

 private:
  struct Rejection {
    JSValue promise;
    JSValue reason;
  };

  static void Free(JSContext* pCtx, const vector<Rejection>& rejections) noexcept;

  vector<Rejection> _rejections;
  JSContext* _pCtx;
  bool _untracked{false};  // a rejection could not be tracked since the previous flush
};

}  // namespace aeronet::internal
