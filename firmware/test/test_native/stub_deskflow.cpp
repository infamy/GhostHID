// Host-test stubs for the DeskflowClient methods that live in the transport TU
// (DeskflowClient.cpp, not compiled for tests). dispatch() and friends are in
// DeskflowDispatch.cpp (compiled); these are the transport calls they make, plus
// the few methods CommandProcessor references when deskflow_ is attached.
#include "net/DeskflowClient.h"
namespace ghosthid {
// referenced by CommandProcessor status/get_config paths
void        DeskflowClient::reconnect() {}
const char *DeskflowClient::statusText() const { return "idle"; }
uint8_t     DeskflowClient::stateCode() const { return 0; }
// transport calls made by dispatch() - no-ops on the host (no socket)
void DeskflowClient::sendCode(const char *) {}
void DeskflowClient::sendScreenInfo() {}
void DeskflowClient::disconnect(const char *) {}
}  // namespace ghosthid
