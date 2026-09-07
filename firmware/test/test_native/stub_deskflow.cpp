// Host-test stub for the few out-of-line DeskflowClient methods CommandProcessor
// references. deskflow_ is null in tests, so these are never called — they exist
// only so the guarded `deskflow_ ? deskflow_->x() : ...` paths link.
#include "net/DeskflowClient.h"
namespace ghosthid {
void        DeskflowClient::reconnect() {}
const char *DeskflowClient::statusText() const { return "idle"; }
uint8_t     DeskflowClient::stateCode() const { return 0; }
}  // namespace ghosthid
