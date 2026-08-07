#pragma once

#include <cstdint>

namespace aeronet {

// RAII helper that guarantees a null-terminated host C-string for its lifetime by temporarily overwriting the last
// character with '\0', restoring it on destruction. While alive, the backing buffer is transiently mutated, so
// the object should be destroyed immediately after usage and the buffer not used for other things.
class TempCStr {
 public:
  TempCStr(char* pStr, uint32_t sz) : _sz(sz), _saved(pStr[_sz]), _pStr(pStr) { _pStr[_sz] = '\0'; }

  TempCStr(const TempCStr&) = delete;
  TempCStr(TempCStr&&) = delete;
  TempCStr& operator=(const TempCStr&) = delete;
  TempCStr& operator=(TempCStr&&) = delete;

  ~TempCStr() { _pStr[_sz] = _saved; }

  // Returns a null-terminated host C-string, valid for the lifetime of the guard.
  [[nodiscard]] const char* c_str() const noexcept { return _pStr; }

 private:
  uint32_t _sz;
  char _saved;
  char* _pStr;
};

}  // namespace aeronet