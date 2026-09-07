// Host-test stub for TlsArena — CommandProcessor only reads its counters for
// the status message; there's no arena on the host.
#include "net/TlsArena.h"

namespace ghosthid {
bool   TlsArena::begin() { return false; }
bool   TlsArena::active() { return false; }
size_t TlsArena::reservedBytes() { return 0; }
size_t TlsArena::inUse() { return 0; }
}  // namespace ghosthid
