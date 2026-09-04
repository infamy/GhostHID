#include "Keymap.h"

#include <string.h>
#include <ctype.h>

#include "hid/HidDevice.h"

namespace ghosthid {
namespace {

struct NamedKey {
    const char *name;
    uint8_t code;
};

// Alias groups are deliberate: controllers on different platforms spell the
// same physical key differently (Windows/Command/Super all mean LeftGui).
const NamedKey kNamedKeys[] = {
    {"ctrl",       key::LeftCtrl},   {"control",    key::LeftCtrl},
    {"lctrl",      key::LeftCtrl},   {"rctrl",      key::RightCtrl},
    {"shift",      key::LeftShift},  {"lshift",     key::LeftShift},
    {"rshift",     key::RightShift},
    {"alt",        key::LeftAlt},    {"lalt",       key::LeftAlt},
    {"ralt",       key::RightAlt},   {"altgr",      key::RightAlt},
    {"gui",        key::LeftGui},    {"win",        key::LeftGui},
    {"windows",    key::LeftGui},    {"cmd",        key::LeftGui},
    {"command",    key::LeftGui},    {"meta",       key::LeftGui},
    {"super",      key::LeftGui},    {"rgui",       key::RightGui},

    {"enter",      key::Return},     {"return",     key::Return},
    {"esc",        key::Escape},     {"escape",     key::Escape},
    {"backspace",  key::Backspace},  {"bksp",       key::Backspace},
    {"tab",        key::Tab},        {"space",      key::Space},
    {"capslock",   key::CapsLock},

    {"insert",     key::Insert},     {"ins",        key::Insert},
    {"delete",     key::Delete},     {"del",        key::Delete},
    {"home",       key::Home},       {"end",        key::End},
    {"pageup",     key::PageUp},     {"pgup",       key::PageUp},
    {"pagedown",   key::PageDown},   {"pgdn",       key::PageDown},

    {"up",         key::UpArrow},    {"uparrow",    key::UpArrow},
    {"down",       key::DownArrow},  {"downarrow",  key::DownArrow},
    {"left",       key::LeftArrow},  {"leftarrow",  key::LeftArrow},
    {"right",      key::RightArrow}, {"rightarrow", key::RightArrow},

    {"f1",  key::F1},  {"f2",  key::F2},  {"f3",  key::F3},  {"f4",  key::F4},
    {"f5",  key::F5},  {"f6",  key::F6},  {"f7",  key::F7},  {"f8",  key::F8},
    {"f9",  key::F9},  {"f10", key::F10}, {"f11", key::F11}, {"f12", key::F12},
};

}  // namespace

uint8_t lookupKey(const char *name) {
    if (name == nullptr || name[0] == '\0') return 0;

    // A single printable character maps to itself; the HID layer synthesises
    // shift for uppercase and shifted punctuation.
    if (name[1] == '\0') {
        const unsigned char c = static_cast<unsigned char>(name[0]);
        return (c >= 0x20 && c <= 0x7E) ? c : 0;
    }

    char lower[24];
    size_t i = 0;
    for (; name[i] != '\0' && i < sizeof(lower) - 1; ++i) {
        lower[i] = static_cast<char>(tolower(static_cast<unsigned char>(name[i])));
    }
    lower[i] = '\0';
    if (name[i] != '\0') return 0;  // longer than any known name

    for (const NamedKey &k : kNamedKeys) {
        if (strcmp(lower, k.name) == 0) return k.code;
    }
    return 0;
}

}  // namespace ghosthid
