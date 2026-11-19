#include "rejection-tracker.hpp"

#include <quickjs.h>

#include <algorithm>
#include <new>
#include <utility>

#include "aeronet/vector.hpp"
#include "js-engine.hpp"

namespace aeronet::internal {

RejectionTracker::~RejectionTracker() { Free(_pCtx, _rejections); }

void RejectionTracker::Free(JSContext* pCtx, const vector<Rejection>& rejections) noexcept {
  for (const Rejection& rejection : rejections) {
    JS_FreeValue(pCtx, rejection.promise);
    JS_FreeValue(pCtx, rejection.reason);
  }
}

void RejectionTracker::track(JSValueConst promise, JSValueConst reason, bool isHandled) noexcept {
  if (isHandled) {
    const auto isPromise = [promise](const Rejection& rejection) {
      return JS_VALUE_GET_PTR(rejection.promise) == JS_VALUE_GET_PTR(promise);
    };
    for (const Rejection& rejection : _rejections) {
      if (isPromise(rejection)) {
        JS_FreeValue(_pCtx, rejection.promise);
        JS_FreeValue(_pCtx, rejection.reason);
      }
    }
    const auto handled = std::ranges::remove_if(_rejections, isPromise);  // in the order of the rejections
    _rejections.erase(handled.begin(), handled.end());
    return;
  }
  try {
    _rejections.push_back({JS_UNDEFINED, JS_UNDEFINED});  // the only step that can throw
  } catch (...) {  // std::bad_alloc, or the std::overflow_error of a vector grown past its size type
    _untracked = true;
    return;
  }
  _rejections.back() = {JS_DupValue(_pCtx, promise), JS_DupValue(_pCtx, reason)};
}

void RejectionTracker::flush(Report report, void* pContext) {
  // Freed even when a report throws.
  class Taken {
   public:
    Taken(JSContext* pCtx, vector<Rejection> rejections) noexcept : _rejections(std::move(rejections)), _pCtx(pCtx) {}

    Taken(const Taken&) = delete;
    Taken& operator=(const Taken&) = delete;
    Taken(Taken&&) = delete;
    Taken& operator=(Taken&&) = delete;

    ~Taken() { Free(_pCtx, _rejections); }

    [[nodiscard]] const auto& rejections() const noexcept { return _rejections; }

   private:
    vector<Rejection> _rejections;
    JSContext* _pCtx;
  };

  const Taken taken(_pCtx, std::exchange(_rejections, {}));
  for (const Rejection& rejection : taken.rejections()) {
    report(pContext, "Uncaught (in promise) " + ExceptionText(_pCtx, rejection.reason));
  }
  if (std::exchange(_untracked, false)) {
    throw std::bad_alloc();
  }
}

}  // namespace aeronet::internal
