#pragma once

#include <cstdlib>
#include <string>

namespace aeronet::test {

class ScopedEnvVar {
 public:
  ScopedEnvVar(const char* name, const char* value) : _name(name) {
    if (const char* prev = std::getenv(name)) {
      _hadOld = true;
      _old = prev;
    }
#ifdef _WIN32
    if (value != nullptr) {
      _putenv_s(name, value);
    } else {
      _putenv_s(name, "");
    }
#else
    if (value != nullptr) {
      ::setenv(name, value, 1);  // NOLINT(misc-include-cleaner) cstdlib header
    } else {
      ::unsetenv(name);  // NOLINT(misc-include-cleaner) cstdlib header
    }
#endif
  }

  ~ScopedEnvVar() {
#ifdef _WIN32
    if (_hadOld) {
      _putenv_s(_name.c_str(), _old.c_str());
    } else {
      _putenv_s(_name.c_str(), "");
    }
#else
    if (_hadOld) {
      ::setenv(_name.c_str(), _old.c_str(), 1);  // NOLINT(misc-include-cleaner) cstdlib header
    } else {
      ::unsetenv(_name.c_str());  // NOLINT(misc-include-cleaner) cstdlib header
    }
#endif
  }

 private:
  std::string _name;
  std::string _old;
  bool _hadOld = false;
};

}  // namespace aeronet::test