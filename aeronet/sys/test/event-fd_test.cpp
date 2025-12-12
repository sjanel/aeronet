#include "aeronet/event-fd.hpp"

#include <dlfcn.h>
#include <gtest/gtest.h>
#include <sys/eventfd.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <optional>
#include <system_error>

#include "aeronet/sys-test-support.hpp"

using namespace aeronet;

namespace {
struct EventfdAction {
  enum class Kind : std::uint8_t { Success, Error };
  Kind kind{Kind::Success};
  int err{0};
};

[[nodiscard]] EventfdAction EventfdErr(int err) { return EventfdAction{EventfdAction::Kind::Error, err}; }

test::ActionQueue<EventfdAction> gCreateActions;
test::ActionQueue<EventfdAction> gWriteActions;
test::ActionQueue<EventfdAction> gReadActions;

void SetCreateActions(std::initializer_list<EventfdAction> actions) { gCreateActions.setActions(actions); }

void SetWriteActions(std::initializer_list<EventfdAction> actions) { gWriteActions.setActions(actions); }

void SetReadActions(std::initializer_list<EventfdAction> actions) { gReadActions.setActions(actions); }

class EventfdHookGuard {
 public:
  EventfdHookGuard() = default;
  EventfdHookGuard(const EventfdHookGuard&) = delete;
  EventfdHookGuard& operator=(const EventfdHookGuard&) = delete;
  ~EventfdHookGuard() {
    gCreateActions.reset();
    gWriteActions.reset();
    gReadActions.reset();
  }
};

}  // namespace

extern "C" int eventfd(unsigned int __count, int __flags) {  // NOLINT(bugprone-reserved-identifier)
  using EventfdFn = int (*)(unsigned int, int);
  static EventfdFn real_eventfd = reinterpret_cast<EventfdFn>(dlsym(RTLD_NEXT, "eventfd"));
  if (real_eventfd == nullptr) {
    std::abort();
  }
  const auto action = gCreateActions.pop();
  if (action.has_value() && action->kind == EventfdAction::Kind::Error) {
    errno = action->err;
    return -1;
  }
  return real_eventfd(__count, __flags);
}

extern "C" int eventfd_write(int __fd, eventfd_t __value) {  // NOLINT(bugprone-reserved-identifier)
  using EventfdWriteFn = int (*)(int, eventfd_t);
  static EventfdWriteFn real_write = reinterpret_cast<EventfdWriteFn>(dlsym(RTLD_NEXT, "eventfd_write"));
  if (real_write == nullptr) {
    std::abort();
  }
  const auto action = gWriteActions.pop();
  if (action.has_value() && action->kind == EventfdAction::Kind::Error) {
    errno = action->err;
    return -1;
  }
  return real_write(__fd, __value);
}

extern "C" int eventfd_read(int __fd, eventfd_t* __value) {  // NOLINT(bugprone-reserved-identifier)
  using EventfdReadFn = int (*)(int, eventfd_t*);
  static EventfdReadFn real_read = reinterpret_cast<EventfdReadFn>(dlsym(RTLD_NEXT, "eventfd_read"));
  if (real_read == nullptr) {
    std::abort();
  }
  const auto action = gReadActions.pop();
  if (action.has_value() && action->kind == EventfdAction::Kind::Error) {
    errno = action->err;
    return -1;
  }
  return real_read(__fd, __value);
}

TEST(EventFdTest, ConstructorThrowsWhenKernelFails) {
  EventfdHookGuard guard;
  SetCreateActions({EventfdErr(EMFILE)});
  EXPECT_THROW(EventFd(), std::system_error);
}

TEST(EventFdTest, SuccessfulSend) {
  EventFd fd;
  EventfdHookGuard guard;
  fd.send();
}

TEST(EventFdTest, SendHandlesEagainWithoutErrorLog) {
  EventFd fd;
  EventfdHookGuard guard;
  SetWriteActions({EventfdErr(EAGAIN)});
  fd.send();
}

TEST(EventFdTest, SendLogsErrors) {
  EventFd fd;
  EventfdHookGuard guard;
  SetWriteActions({EventfdErr(EIO)});
  fd.send();
}

TEST(EventFdTest, SuccessfulRead) {
  EventFd fd;
  EventfdHookGuard guard;
  fd.read();
}

TEST(EventFdTest, ReadHandlesEagainWithoutErrorLog) {
  EventFd fd;
  EventfdHookGuard guard;
  SetReadActions({EventfdErr(EAGAIN)});
  fd.read();
}

TEST(EventFdTest, ReadLogsErrors) {
  EventFd fd;
  EventfdHookGuard guard;
  SetReadActions({EventfdErr(EIO)});
  fd.read();
}
