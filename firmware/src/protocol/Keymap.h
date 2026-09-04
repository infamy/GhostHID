// Maps human-readable key names ("CTRL", "ENTER", "a") to HID keycodes.
//
// Pure logic with no USB or network dependency, so it is unit-testable on the
// host. See tests/.

#pragma once

#include <stdint.h>

namespace ghosthid {

// Resolves a key name to a keycode. Accepts:
//   * a single printable ASCII character: "a", "A", "7", "/"
//   * a named key, case-insensitive: "ENTER", "esc", "F5", "LeftArrow"
//   * common aliases: "CTRL"/"CONTROL", "WIN"/"CMD"/"GUI"/"META", "RETURN"
// Returns 0 if the name is not recognised (0 is not a valid keycode).
uint8_t lookupKey(const char *name);

}  // namespace ghosthid
