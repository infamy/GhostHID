// Observability hooks the recording HidDevice stub updates, so tests can assert
// on what CommandProcessor asked the HID layer to do.
#pragma once
#include <stdint.h>

namespace hidhook {
extern int      releaseAllCalls;
extern int      keyDownCalls;
extern int      keyUpCalls;
extern int      typeTextCalls;
extern int      mouseMoveCalls;
extern int      mouseAbsCalls;
extern int      mouseWheelCalls;
extern int      mouseBtnDownCalls;
extern int      mediaCalls;
extern int      systemCalls;
extern uint8_t  lastKeyDown;
extern bool     ready;        // test-settable USB-ready state
void reset();
}  // namespace hidhook
