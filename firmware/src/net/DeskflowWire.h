// Deskflow / Barrier / Synergy wire primitives — big-endian field readers and
// the untrusted-KeyID -> HID keycode map.
//
// Pulled out of DeskflowClient (which drags the whole TLS/Wi-Fi stack) so the
// parsing of bytes that arrive off the network can be unit-tested on the host.
// Pure functions and a constant table; no I/O.

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "hid/HidDevice.h"   // key:: constants

namespace ghosthid {

// Big-endian field readers (the protocol is entirely network byte order).
inline uint16_t rd16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
inline int16_t  rdS16(const uint8_t *p) { return (int16_t)rd16(p); }
inline uint32_t rd32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

// True if the message begins with the 4-char code (and is at least that long).
inline bool is(const uint8_t *m, size_t len, const char *code) {
    return len >= 4 && memcmp(m, code, 4) == 0;
}

struct KeyMap { uint16_t keyId; uint8_t hid; };

inline constexpr KeyMap kSpecialKeys[] = {
    {0xEF08, key::Backspace}, {0xEF09, key::Tab},       {0xEF0D, key::Return},
    {0xEF1B, key::Escape},    {0xEFFF, key::Delete},    {0xEF50, key::Home},
    {0xEF51, key::LeftArrow}, {0xEF52, key::UpArrow},   {0xEF53, key::RightArrow},
    {0xEF54, key::DownArrow}, {0xEF55, key::PageUp},    {0xEF56, key::PageDown},
    {0xEF57, key::End},       {0xEF63, key::Insert},    {0xEFE5, key::CapsLock},
    {0xEF8D, key::Return},    {0xEF80, key::Space},     {0xEF89, key::Tab},

    {0xEFE1, key::LeftShift}, {0xEFE2, key::RightShift},
    {0xEFE3, key::LeftCtrl},  {0xEFE4, key::RightCtrl},
    {0xEFE9, key::LeftAlt},   {0xEFEA, key::RightAlt},
    {0xEF7E, key::RightAlt},                            // AltGr
    {0xEFE7, key::LeftGui},   {0xEFE8, key::RightGui},
    {0xEFEB, key::LeftGui},   {0xEFEC, key::RightGui},

    {0xEFBE, key::F1},  {0xEFBF, key::F2},  {0xEFC0, key::F3},  {0xEFC1, key::F4},
    {0xEFC2, key::F5},  {0xEFC3, key::F6},  {0xEFC4, key::F7},  {0xEFC5, key::F8},
    {0xEFC6, key::F9},  {0xEFC7, key::F10}, {0xEFC8, key::F11}, {0xEFC9, key::F12},
};

// Maps a Synergy/Barrier KeyID (untrusted, straight off the network) to a HID
// keycode, or 0 if it maps to nothing we send.
inline uint8_t keyIdToHid(uint16_t keyId) {
    if (keyId >= 0x20 && keyId <= 0x7E) return (uint8_t)keyId;   // printable ASCII
    for (const KeyMap &k : kSpecialKeys) {
        if (k.keyId == keyId) return k.hid;
    }
    // Fallback for a server that sets high bits on an otherwise plain character.
    const uint16_t low = keyId & 0x00FF;
    if ((keyId & 0xFF00) != 0 && low >= 0x20 && low <= 0x7E &&
        (keyId & 0xFF00) != 0xEF00) {
        return (uint8_t)low;
    }
    return 0;
}

}  // namespace ghosthid
