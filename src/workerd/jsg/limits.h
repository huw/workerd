#pragma once

#include <kj/common.h>

namespace workerd::jsg {

class Lock;

class IsolateLimitEnforcer {
public:
  // Called when performing a cypto key derivation function (like pbkdf2) to determine if
  // if the requested number of iterations is acceptable. If kj::none is returned, the
  // number of iterations requested is acceptable. If a number is returned, the requested
  // iterations is unacceptable and the return value specifies the maximum.
  virtual kj::Maybe<size_t> checkKdfIterations(Lock& js, size_t iterations) = 0;
};

}  // namespace workerd::jsg
