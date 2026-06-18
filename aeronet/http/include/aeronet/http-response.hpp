#pragma once

#include "aeronet/http-message.hpp"

namespace aeronet {

// HttpMessage was historically named HttpResponse (it is the contiguous, HTTP/1.1-optimised buffer
// for a full message head + body). The name is retained as an alias for backward compatibility and
// because it remains the natural type for server responses.
using HttpResponse = HttpMessage;

}  // namespace aeronet
